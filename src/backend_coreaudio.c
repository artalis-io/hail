#include "hail_internal.h"
#include <CoreAudio/CoreAudio.h>
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <time.h>
ha_result hail_coreaudio_capture_permission(void);
#if HA_COREAUDIO_DIAGNOSTICS
#include "coreaudio_diagnostics.h"
#include <mach/mach_time.h>
#endif

typedef struct ca_context { ha_context *owner; atomic_uint callbacks; uint32_t listeners; AudioDeviceID defaults[2]; } ca_context;
typedef struct ca_leg { ha_stream *owner; AudioUnit unit; uint32_t leg, initialized, started; atomic_uint callbacks; } ca_leg;
typedef struct ca_stream {
#if HA_COREAUDIO_DIAGNOSTICS
    /* First member for source-private qualification snapshots after retire. */
    hail_ca_trace trace;
#endif
    ca_leg legs[2];
    atomic_uint callbacks;
    uint32_t flushing, flush_running;
    unsigned char input[HA_MAX_QUANTUM * 8u];
} ca_stream;

#if HA_COREAUDIO_DIAGNOSTICS
static uint64_t ca_trace_now(void) { return AudioConvertHostTimeToNanos(mach_absolute_time()); }
static void ca_trace_callback(ca_stream *a, uint32_t leg, uint32_t frames,
                              uint32_t rate, uint64_t entry)
{
    hail_ca_callback_trace *t = &a->trace.leg[leg];
    uint64_t duration = ca_trace_now() - entry, bin = duration / 10000u;
    ++t->calls; t->frames += frames; t->duration_ns += duration;
    if (duration > t->max_duration_ns) t->max_duration_ns = duration;
    ++t->duration_hist[bin < HAIL_CA_TRACE_BINS ? bin : HAIL_CA_TRACE_BINS - 1u];
    if (t->last_entry_ns) {
        uint64_t interval = entry - t->last_entry_ns;
        uint64_t jitter = interval > t->last_period_ns ? interval - t->last_period_ns : t->last_period_ns - interval;
        ++t->interval_count; t->interval_ns += interval;
        if (interval > t->max_interval_ns) t->max_interval_ns = interval;
        if (jitter > t->max_jitter_ns) t->max_jitter_ns = jitter;
        bin = jitter / 10000u;
        ++t->jitter_hist[bin < HAIL_CA_TRACE_BINS ? bin : HAIL_CA_TRACE_BINS - 1u];
    }
    t->last_entry_ns = entry;
    t->last_period_ns = (uint64_t)frames * 1000000000u / rate;
}
#define CA_CONTROL(a, index, call) do { \
    uint64_t ca_begin = ca_trace_now(), ca_elapsed; \
    hail_ca_control_trace *ca_t = &(a)->trace.flush[index]; \
    status = (call); ca_elapsed = ca_trace_now() - ca_begin; \
    ++ca_t->calls; ca_t->total_ns += ca_elapsed; ca_t->last_status = status; \
    if (ca_elapsed > ca_t->max_ns) ca_t->max_ns = ca_elapsed; \
} while (0)
#else
#define CA_CONTROL(a, index, call) do { status = (call); } while (0)
#endif

static ha_result ca_result(OSStatus status)
{
    if (!status) return HA_OK;
    if (status == kAudioUnitErr_Unauthorized || status == kAudioComponentErr_NotPermitted ||
        status == kAudioDevicePermissionsError) return HA_PERMISSION_DENIED;
    if (status == kAudioHardwareBadDeviceError || status == kAudioHardwareBadObjectError) return HA_DEVICE_LOST;
    if (status == kAudioUnitErr_FormatNotSupported || status == kAudioUnitErr_InvalidPropertyValue) return HA_FORMAT_UNSUPPORTED;
    return HA_STREAM_FAILED;
}
static OSStatus ca_get(AudioObjectID object, AudioObjectPropertySelector selector,
                         AudioObjectPropertyScope scope, void *value, UInt32 *bytes)
{
    AudioObjectPropertyAddress address = {selector, scope, 0};
    return AudioObjectGetPropertyData(object, &address, 0, NULL, bytes, value);
}
static int ca_string(AudioObjectID object, AudioObjectPropertySelector selector, char *out, size_t capacity)
{
    CFStringRef string = NULL; UInt32 bytes = sizeof(string); int result = 0;
    if (!ca_get(object, selector, kAudioObjectPropertyScopeGlobal, &string, &bytes) && string) {
        result = CFStringGetCString(string, out, (CFIndex)capacity, kCFStringEncodingUTF8);
        CFRelease(string);
    }
    return result;
}
static uint32_t ca_channels(AudioDeviceID device, uint32_t leg)
{
    _Alignas(16) unsigned char buffer[4096];
    AudioBufferList *list = (AudioBufferList *)(void *)buffer;
    UInt32 bytes = sizeof(buffer), channels = 0, i;
    if (ca_get(device, kAudioDevicePropertyStreamConfiguration,
               leg ? kAudioDevicePropertyScopeOutput : kAudioDevicePropertyScopeInput, list, &bytes)) return 0;
    if (bytes < offsetof(AudioBufferList, mBuffers) || list->mNumberBuffers >
        (bytes - offsetof(AudioBufferList, mBuffers)) / sizeof(AudioBuffer)) return 0;
    for (i = 0; i < list->mNumberBuffers; ++i) {
        if (list->mBuffers[i].mNumberChannels > 1024u - channels) return 0;
        channels += list->mBuffers[i].mNumberChannels;
    }
    return channels;
}
static OSStatus ca_changed(AudioObjectID object, UInt32 count, const AudioObjectPropertyAddress *addresses, void *user)
{
    ca_context *p = user; (void)object; (void)count; (void)addresses;
    atomic_fetch_add_explicit(&p->callbacks, 1, memory_order_acquire);
    atomic_store_explicit(&p->owner->registry_dirty, 1, memory_order_release);
    atomic_fetch_sub_explicit(&p->callbacks, 1, memory_order_release); return noErr;
}
static const AudioObjectPropertySelector ca_properties[] = {
    kAudioHardwarePropertyDevices, kAudioHardwarePropertyDefaultInputDevice, kAudioHardwarePropertyDefaultOutputDevice
};
static ha_result ca_refresh(ha_context *c)
{
    AudioDeviceID devices[HA_MAX_DEVICES];
    hail_device records[HA_MAX_DEVICES];
    UInt32 bytes = sizeof(devices), i, count = 0, leg;
    char defaults[2][1024] = {{0}};
    OSStatus error = ca_get(kAudioObjectSystemObject, kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, devices, &bytes);
    if (error) return ca_result(error);
    if (bytes > sizeof(devices) || bytes % sizeof(AudioDeviceID)) return HA_RESOURCE_EXHAUSTED;
    for (i = 0; i < bytes / sizeof(AudioDeviceID); ++i) {
        char uid[1024], name[1024];
        if (!ca_string(devices[i], kAudioDevicePropertyDeviceUID, uid, sizeof(uid))) return HA_RESOURCE_EXHAUSTED;
        if (!ca_string(devices[i], kAudioObjectPropertyName, name, sizeof(name))) memcpy(name, uid, strlen(uid) + 1u);
        for (leg = 0; leg < 2; ++leg) if (ca_channels(devices[i], leg)) {
            ha_result r;
            if (count == c->max_devices) return HA_RESOURCE_EXHAUSTED;
            r = hail_device_record(&records[count], leg ? HA_PLAYBACK : HA_CAPTURE, uid, name, devices[i]);
            if (r != HA_OK) return r;
            hail_device_persistent(&records[count]);
            ++count;
        }
    }
    for (leg = 0; leg < 2; ++leg) {
        AudioDeviceID device = 0; UInt32 n = sizeof(device);
        if (!ca_get(kAudioObjectSystemObject, leg ? kAudioHardwarePropertyDefaultOutputDevice : kAudioHardwarePropertyDefaultInputDevice,
                    kAudioObjectPropertyScopeGlobal, &device, &n)) (void)ca_string(device, kAudioDevicePropertyDeviceUID, defaults[leg], sizeof(defaults[leg]));
        ((ca_context *)c->backend_state)->defaults[leg] = device;
    }
    return hail_registry_publish(c, records, count, defaults[0], defaults[1]);
}
static ha_result ca_context_init(ha_context *c)
{
    ca_context *p = c->backend_state; uint32_t i;
    ha_result r;
    p->owner = c; atomic_init(&p->callbacks, 0);
    r = ca_refresh(c);
    if (r != HA_OK) return r;
    for (i = 0; i < 1; ++i) {
        AudioObjectPropertyAddress address = {ca_properties[i], kAudioObjectPropertyScopeGlobal, 0};
        OSStatus status = AudioObjectAddPropertyListener(kAudioObjectSystemObject, &address, ca_changed, p);
        if (status) return ca_result(status);
        ++p->listeners;
    }
    return HA_OK;
}
static ha_result ca_context_free(ha_context *c)
{
    ca_context *p = c->backend_state; uint32_t i;
    for (i = 0; i < p->listeners; ++i) {
        AudioObjectPropertyAddress address = {ca_properties[i], kAudioObjectPropertyScopeGlobal, 0};
        if (AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &address, ca_changed, p)) return HA_PENDING;
    }
    p->listeners = 0;
    return atomic_load_explicit(&p->callbacks, memory_order_acquire) ? HA_PENDING : HA_OK;
}
static void ca_context_poll(ha_context *c)
{
    ca_context *p = c->backend_state; uint32_t leg, i;
    for (leg = 0; leg < 2; ++leg) {
        AudioDeviceID device = 0; UInt32 bytes = sizeof(device);
        if (!ca_get(kAudioObjectSystemObject, ca_properties[leg + 1u], kAudioObjectPropertyScopeGlobal, &device, &bytes) && device != p->defaults[leg]) {
            p->defaults[leg] = device; atomic_store_explicit(&c->registry_dirty, 1, memory_order_release);
        }
    }
    /* Observe a known denial/revocation on the control owner. Never query
     * authorization on an audio callback or fall back to another endpoint. */
    for (i = 0; i < c->max_streams; ++i) if (c->streams[i] && c->streams[i]->info.capture_slots) {
        if (hail_coreaudio_capture_permission() == HA_PERMISSION_DENIED) {
            uint32_t j;
            for (j = i; j < c->max_streams; ++j) if (c->streams[j] && c->streams[j]->info.capture_slots)
                hail_rt_error(c->streams[j], 0, HA_PERMISSION_DENIED, 0);
        }
        break;
    }
}
static int ca_native_format(AudioDeviceID device, uint32_t leg, ha_format *format)
{
    AudioStreamID streams[64]; UInt32 bytes = sizeof(streams);
    AudioStreamBasicDescription asbd;
    if (ca_get(device, kAudioDevicePropertyStreams, leg ? kAudioDevicePropertyScopeOutput : kAudioDevicePropertyScopeInput, streams, &bytes) ||
        bytes != sizeof(AudioStreamID)) return 0;
    bytes = sizeof(asbd);
    if (ca_get(streams[0], kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, &asbd, &bytes) ||
        asbd.mFormatID != kAudioFormatLinearPCM || asbd.mSampleRate < 1 || asbd.mSampleRate > 192000 ||
        asbd.mFormatFlags & kAudioFormatFlagIsBigEndian) return 0;
    memset(format, 0, sizeof(*format)); format->struct_size = sizeof(*format); format->struct_version = HA_STRUCT_VERSION;
    format->sample_rate = (uint32_t)asbd.mSampleRate;
    if ((double)format->sample_rate != asbd.mSampleRate) return 0;
    if ((asbd.mFormatFlags & kAudioFormatFlagIsFloat) && asbd.mBitsPerChannel == 32) format->sample_format = HA_PCM_F32;
    else if ((asbd.mFormatFlags & kAudioFormatFlagIsSignedInteger) && asbd.mBitsPerChannel == 16) format->sample_format = HA_PCM_S16;
    else return 0;
    format->channels = asbd.mChannelsPerFrame;
    format->channel_layout = format->channels == 1 ? HA_LAYOUT_MONO : format->channels == 2 ? HA_LAYOUT_STEREO : 0;
    format->storage = asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved ? HA_PLANAR : HA_INTERLEAVED;
    return 1;
}
static ha_result ca_probe(ha_context *c, uint64_t id, const ha_format *f, uint32_t negotiation, ha_probe_info *out)
{
    hail_device *d = hail_device_find(c, id); Float64 rate = 0; UInt32 bytes = sizeof(rate);
    uint32_t leg; OSStatus status;
    if (!d) return HA_DEVICE_UNAVAILABLE;
    leg = d->info.direction == HA_CAPTURE ? 0u : 1u;
    if (!leg && hail_coreaudio_capture_permission() == HA_PERMISSION_DENIED) return HA_PERMISSION_DENIED;
    status = ca_get((AudioDeviceID)d->native_id, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &rate, &bytes);
    if (status) return ca_result(status);
    if (rate != (double)f->sample_rate) return HA_FORMAT_UNSUPPORTED; /* AUHAL does not change device clock rate. */
    if (f->channels > ca_channels((AudioDeviceID)d->native_id, leg)) return HA_FORMAT_UNSUPPORTED;
    out->application_format = *f; out->backend_format = *f; out->backend_format_valid = 1;
    out->device_format_valid = (uint32_t)ca_native_format((AudioDeviceID)d->native_id, leg, &out->device_format);
    out->conversion = HA_CONVERSION_NATIVE;
    out->capabilities = HA_FEATURE_RENDER_REFERENCE | HA_FEATURE_NATIVE_FLUSH;
    if (out->device_format_valid && out->device_format.sample_format == f->sample_format && out->device_format.channels == f->channels &&
        out->device_format.storage == f->storage && out->device_format.sample_rate == f->sample_rate) out->conversion = HA_CONVERSION_NONE;
    if (negotiation == HA_EXACT_END_TO_END && out->conversion != HA_CONVERSION_NONE) return HA_UNSUPPORTED;
    return HA_OK;
}

static OSStatus ca_audio(void *user, AudioUnitRenderActionFlags *action, const AudioTimeStamp *timestamp,
                           UInt32 bus, UInt32 frames, AudioBufferList *output)
{
    ca_leg *l = user; ha_stream *s = l->owner; ca_stream *a = s->backend_state;
    uint32_t bpf = l->leg ? s->playback.bytes_per_frame : s->capture.bytes_per_frame, offset = 0;
    uint64_t host = 0, position = 0; uint32_t flags = 0; int entered = 0;
    AudioBufferList input; OSStatus status = noErr; unsigned char *data = NULL;
#if HA_COREAUDIO_DIAGNOSTICS
    uint64_t trace_entry = ca_trace_now();
#endif
    (void)bus;
    atomic_fetch_add_explicit(&a->callbacks, 1, memory_order_acquire);
    atomic_fetch_add_explicit(&l->callbacks, 1, memory_order_acquire);
    if (frames > HA_MAX_QUANTUM || !bpf) { status = kAudioUnitErr_TooManyFramesToProcess; goto done; }
    if (l->leg) {
        if (!output || output->mNumberBuffers != 1 || !output->mBuffers[0].mData || output->mBuffers[0].mDataByteSize < frames * bpf ||
            output->mBuffers[0].mNumberChannels != s->info.playback_format.channels) {
            status = kAudioUnitErr_InvalidParameter; goto done;
        }
        data = output->mBuffers[0].mData;
    }
    entered = hail_rt_enter(s, l->leg);
    if (!entered) { if (data) { memset(data, 0, frames * bpf); *action |= kAudioUnitRenderAction_OutputIsSilence; } goto done; }
    if (!l->leg) {
        input.mNumberBuffers = 1; input.mBuffers[0].mNumberChannels = s->info.capture_format.channels;
        input.mBuffers[0].mDataByteSize = frames * bpf; input.mBuffers[0].mData = a->input;
        status = AudioUnitRender(l->unit, action, timestamp, 1, frames, &input);
        if (status) goto done;
        if (input.mNumberBuffers != 1 || !input.mBuffers[0].mData || input.mBuffers[0].mDataByteSize < frames * bpf ||
            input.mBuffers[0].mNumberChannels != s->info.capture_format.channels) { status = kAudioUnitErr_InvalidParameter; goto done; }
        data = input.mBuffers[0].mData;
    }
    if (timestamp && (timestamp->mFlags & kAudioTimeStampHostTimeValid)) {
        host = AudioConvertHostTimeToNanos(timestamp->mHostTime); flags |= HA_TIME_HOST_VALID;
    }
    if (timestamp && (timestamp->mFlags & kAudioTimeStampSampleTimeValid) && timestamp->mSampleTime >= 0 &&
        timestamp->mSampleTime < 18446744073709549568.0) { position = (uint64_t)timestamp->mSampleTime; flags |= HA_TIME_DEVICE_VALID; }
    while (offset < frames) {
        uint32_t n = frames - offset; ha_result r;
        uint64_t delta = (uint64_t)offset * UINT64_C(1000000000) /
            (l->leg ? s->info.playback_format.sample_rate : s->info.capture_format.sample_rate);
        if (n > s->info.quantum_frames) n = s->info.quantum_frames;
        if (delta > UINT64_MAX - host || position > UINT64_MAX - offset) { status = kAudioUnitErr_InvalidParameter; break; }
        if (l->leg) r = hail_native_render(s, data + (size_t)offset * bpf, n, host + delta, position + offset, UINT64_MAX, flags);
        else r = hail_native_capture(s, data + (size_t)offset * bpf, n, host + delta, position + offset, UINT64_MAX, flags);
        if (r != HA_OK) { hail_rt_error(s, l->leg, r, 0); break; }
        offset += n;
    }
done:
    if (status) hail_rt_error(s, l->leg, ca_result(status), status);
    if (entered) {
#if HA_COREAUDIO_DIAGNOSTICS
        ca_trace_callback(a, l->leg, frames,
            l->leg ? s->info.playback_format.sample_rate : s->info.capture_format.sample_rate, trace_entry);
#endif
        hail_rt_leave(s, l->leg);
    }
    atomic_fetch_sub_explicit(&l->callbacks, 1, memory_order_release);
    atomic_fetch_sub_explicit(&a->callbacks, 1, memory_order_release);
    return status;
}

static OSStatus ca_unit_open(ha_stream *s, ca_leg *l, AudioDeviceID device, uint32_t leg)
{
    AudioComponentDescription description = {kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent component = AudioComponentFindNext(NULL, &description);
    AudioStreamBasicDescription format = {0}; AURenderCallbackStruct callback;
    const ha_format *f = leg ? &s->info.playback_format : &s->info.capture_format;
    UInt32 enabled = 1, disabled = 0, maximum = HA_MAX_QUANTUM;
    OSStatus status;
    if (!component) return kAudioUnitErr_InvalidProperty;
    l->owner = s; l->leg = leg;
    status = AudioComponentInstanceNew(component, &l->unit);
    if (status) return status;
    if (!leg) {
        status = AudioUnitSetProperty(l->unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &enabled, sizeof(enabled));
        if (!status) status = AudioUnitSetProperty(l->unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &disabled, sizeof(disabled));
    }
    if (!status) status = AudioUnitSetProperty(l->unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device, sizeof(device));
    format.mSampleRate = f->sample_rate; format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsPacked | (f->sample_format == HA_PCM_S16 ? kAudioFormatFlagIsSignedInteger : kAudioFormatFlagIsFloat);
    format.mBitsPerChannel = f->sample_format == HA_PCM_S16 ? 16 : 32;
    format.mChannelsPerFrame = f->channels; format.mFramesPerPacket = 1;
    format.mBytesPerFrame = f->channels * format.mBitsPerChannel / 8u; format.mBytesPerPacket = format.mBytesPerFrame;
    if (!status) status = AudioUnitSetProperty(l->unit, kAudioUnitProperty_StreamFormat, leg ? kAudioUnitScope_Input : kAudioUnitScope_Output,
                                               leg ? 0u : 1u, &format, sizeof(format));
    if (!status) status = AudioUnitSetProperty(l->unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maximum, sizeof(maximum));
    callback.inputProc = ca_audio; callback.inputProcRefCon = l;
    if (!status) status = AudioUnitSetProperty(l->unit, leg ? kAudioUnitProperty_SetRenderCallback : kAudioOutputUnitProperty_SetInputCallback,
                                               leg ? kAudioUnitScope_Input : kAudioUnitScope_Global, 0, &callback, sizeof(callback));
    if (!status && !leg) status = AudioUnitSetProperty(l->unit, kAudioUnitProperty_ShouldAllocateBuffer, kAudioUnitScope_Output, 1, &disabled, sizeof(disabled));
    if (!status) status = AudioUnitInitialize(l->unit);
    if (!status) l->initialized = 1;
    return status;
}
static void ca_stream_free(ha_stream *s)
{
    ca_stream *a = s->backend_state; uint32_t i;
    for (i = 0; i < 2; ++i) if (a->legs[i].unit) {
        AudioOutputUnitStop(a->legs[i].unit);
        AudioUnitUninitialize(a->legs[i].unit);
        AudioComponentInstanceDispose(a->legs[i].unit); a->legs[i].unit = NULL;
    }
}
static ha_result ca_open(ha_stream *s)
{
    ca_stream *a = s->backend_state; uint32_t i; AudioDeviceID ids[2] = {0,0};
    atomic_init(&a->callbacks, 0);
    atomic_init(&a->legs[0].callbacks, 0); atomic_init(&a->legs[1].callbacks, 0);
    for (i = 0; i < 2; ++i) {
        uint64_t id = i ? s->info.playback_device_id : s->info.capture_device_id;
        hail_device *device; OSStatus status;
        if (!id) continue;
        device = hail_device_find(s->context, id);
        if (!device) { ca_stream_free(s); return HA_DEVICE_UNAVAILABLE; }
        ids[i] = (AudioDeviceID)device->native_id;
        status = ca_unit_open(s, &a->legs[i], ids[i], i);
        if (status) { ca_stream_free(s); return ca_result(status); }
    }
    if (ids[0] && ids[0] == ids[1]) {
        UInt32 domain = 0, bytes = sizeof(domain);
        if (!ca_get(ids[0], kAudioDevicePropertyClockDomain, kAudioObjectPropertyScopeGlobal, &domain, &bytes) && domain) {
            s->info.clock_relationship = HA_CLOCK_SHARED; s->info.playback_clock_domain = s->info.capture_clock_domain;
        }
    }
    return HA_OK;
}
static ha_result ca_start(ha_stream *s)
{
    ca_stream *a = s->backend_state; uint32_t i;
    if (s->info.capture_slots && hail_coreaudio_capture_permission() == HA_PERMISSION_DENIED) return HA_PERMISSION_DENIED;
    for (i = 0; i < 2; ++i) if (a->legs[i].unit) {
        OSStatus status = AudioOutputUnitStart(a->legs[i].unit);
        if (status) {
            uint32_t j;
            /* A failed rollback stop is not proof of quiescence. Retain that
             * leg as possibly started so stop/retire retries the SDK call.
             * Also stop the failed-start leg: native partial start is unknown. */
            for (j = 0; j <= i; ++j) if (a->legs[j].unit)
                a->legs[j].started = AudioOutputUnitStop(a->legs[j].unit) ? 1u : 0u;
            return ca_result(status);
        }
        a->legs[i].started = 1;
    }
    return HA_OK;
}
static ha_result ca_stop(ha_stream *s)
{
    ca_stream *a = s->backend_state; uint32_t i; ha_result result = HA_OK;
    a->flushing = 0; a->flush_running = 0;
    for (i = 0; i < 2; ++i) if (a->legs[i].unit && a->legs[i].started) {
        OSStatus status = AudioOutputUnitStop(a->legs[i].unit);
        if (status) result = ca_result(status); else a->legs[i].started = 0;
    }
    return atomic_load_explicit(&a->callbacks, memory_order_acquire) ? HA_PENDING : result;
}
static ha_result ca_retire(ha_stream *s)
{
    ca_stream *a = s->backend_state; uint32_t i;
    (void)ca_stop(s);
    for (i = 0; i < 2; ++i) if (a->legs[i].initialized) {
        OSStatus status = AudioUnitUninitialize(a->legs[i].unit);
        if (status) return ca_result(status);
        a->legs[i].initialized = 0;
    }
    return atomic_load_explicit(&a->callbacks, memory_order_acquire) ? HA_PENDING : HA_OK;
}
static ha_result ca_flush(ha_stream *s)
{
    ca_stream *a = s->backend_state; ca_leg *l = &a->legs[1]; OSStatus status = noErr;
    if (!a->flushing) {
        a->flush_running = l->started;
        if (l->started) { CA_CONTROL(a, 0, AudioOutputUnitStop(l->unit)); }
        if (status) return ca_result(status);
        l->started = 0; a->flushing = 1;
    }
    if (atomic_load_explicit(&l->callbacks, memory_order_acquire)) return HA_PENDING;
    if (!status) { CA_CONTROL(a, 1, AudioUnitReset(l->unit, kAudioUnitScope_Global, 0)); }
    if (!status && a->flush_running) { CA_CONTROL(a, 2, AudioOutputUnitStart(l->unit)); if (!status) l->started = 1; }
    a->flushing = 0;
    return ca_result(status);
}
const hail_backend_ops hail_coreaudio_backend = {
    .id = HA_BACKEND_COREAUDIO, .features = HA_FEATURE_COREAUDIO | HA_FEATURE_RENDER_REFERENCE | HA_FEATURE_NATIVE_FLUSH,
    .context_init = ca_context_init, .probe = ca_probe, .open = ca_open,
    .start = ca_start, .stop = ca_stop, .flush = ca_flush, .retire = ca_retire,
    .context_bytes = sizeof(ca_context), .stream_bytes = sizeof(ca_stream),
    .refresh = ca_refresh, .context_free = ca_context_free, .stream_free = ca_stream_free,
    .context_poll = ca_context_poll
};
