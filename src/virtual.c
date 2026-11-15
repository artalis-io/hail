#include "hail_internal.h"
#include <hail/virtual.h>

static ha_result add_device(ha_context *c, uint32_t direction, uint32_t permitted, uint64_t *id)
{
    hail_device *d;
    const char *name = direction == HA_CAPTURE ? "Virtual capture" : "Virtual playback";
    if (c->device_count == c->max_devices) return HA_RESOURCE_EXHAUSTED;
    if (c->next_id == UINT64_MAX) return HA_OVERFLOW;
    d = &c->devices[c->device_count++];
    memset(d, 0, sizeof(*d));
    d->info.struct_size = sizeof(ha_device_info); d->info.struct_version = HA_STRUCT_VERSION;
    d->info.runtime_id = c->next_id++; d->info.direction = direction;
    d->info.backend = HA_BACKEND_VIRTUAL; d->info.capabilities = c->backend->features;
    d->info.name_bytes = (uint32_t)strlen(name);
    memcpy(d->info.name, name, d->info.name_bytes + 1u);
    d->permitted = permitted; *id = d->info.runtime_id;
    return HA_OK;
}

static ha_result virtual_context_init(ha_context *c)
{
    ha_result r;
    if (c->max_devices < 2) return HA_RESOURCE_EXHAUSTED;
    r = add_device(c, HA_CAPTURE, 1, &c->default_id[0]);
    if (r != HA_OK) return r;
    return add_device(c, HA_PLAYBACK, 1, &c->default_id[1]);
}

static ha_result virtual_probe(ha_context *c, uint64_t id, const ha_format *f,
                                 uint32_t negotiation, ha_probe_info *out)
{
    (void)id; (void)negotiation;
    out->application_format = *f; out->device_format = *f;
    out->device_format_valid = 1; out->conversion = 0;
    out->capabilities = c->backend->features;
    return HA_OK;
}

static ha_result virtual_open(ha_stream *s)
{
    s->info.clock_relationship = HA_CLOCK_INDEPENDENT;
    return HA_OK;
}

static ha_result virtual_start(ha_stream *s)
{
    if (s->faults & HA_VIRTUAL_PERMISSION_DENIED) return HA_PERMISSION_DENIED;
    if (s->faults & HA_VIRTUAL_FAIL_START) return HA_STREAM_FAILED;
    if (s->control_ticks < s->start_delay) { ++s->control_ticks; return HA_PENDING; }
    s->clock_ns[0] = s->host_ns; s->clock_ns[1] = s->host_ns;
    s->clock_remainder[0] = 0; s->clock_remainder[1] = 0;
    return HA_OK;
}

static ha_result virtual_stop(ha_stream *s)
{
    if (s->faults & HA_VIRTUAL_HOLD_RETIREMENT) return HA_PENDING;
    if (s->faults & HA_VIRTUAL_FAIL_STOP) return HA_STREAM_FAILED;
    if (s->control_ticks < s->stop_delay) { ++s->control_ticks; return HA_PENDING; }
    return HA_OK;
}

static ha_result virtual_flush(ha_stream *s)
{
    if (s->control_ticks < s->flush_delay) { ++s->control_ticks; return HA_PENDING; }
    if (s->faults & HA_VIRTUAL_FAIL_FLUSH) return HA_STREAM_FAILED;
    /* Virtual processing and poll share the serialized driver role: no native
     * submission can still be on the stack here. The consumer owns the cursor. */
    s->playback.offset = 0;
    atomic_store_explicit(&s->playback.read_index, s->flush_cutoff, memory_order_release);
    if (s->flush_flags & HA_FLUSH_NATIVE_REQUIRED) {
        if (s->playback_epoch == UINT64_MAX) return HA_OVERFLOW;
        ++s->playback_epoch; s->playback_position = 0;
        s->clock_ns[1] = s->host_ns; s->clock_remainder[1] = 0;
        s->playback_flags |= HA_BLOCK_DISCONTINUITY;
    }
    return HA_OK;
}

static ha_result virtual_retire(ha_stream *s)
{
    if (s->faults & HA_VIRTUAL_HOLD_RETIREMENT) return HA_PENDING;
    return HA_OK;
}

const hail_backend_ops hail_virtual_backend = {
    HA_BACKEND_VIRTUAL,
    HA_FEATURE_VIRTUAL | HA_FEATURE_RENDER_REFERENCE | HA_FEATURE_NATIVE_FLUSH,
    virtual_context_init, virtual_probe, virtual_open, virtual_start, virtual_stop, virtual_flush, virtual_retire
};

ha_result HA_CALL ha_virtual_configure(ha_stream *s, const ha_virtual_config *v)
{
    uint32_t state;
    if (!s || !HAIL_HEADER(v) || s->context->backend->id != HA_BACKEND_VIRTUAL ||
        (v->faults & ~(HA_VIRTUAL_FAIL_START | HA_VIRTUAL_FAIL_STOP | HA_VIRTUAL_FAIL_FLUSH |
                      HA_VIRTUAL_HOLD_RETIREMENT | HA_VIRTUAL_PERMISSION_DENIED)) ||
        v->capture_drift_ppm < -1000 || v->capture_drift_ppm > 1000 ||
        v->playback_drift_ppm < -1000 || v->playback_drift_ppm > 1000)
        return HA_INVALID_ARGUMENT;
    state = atomic_load_explicit(&s->state, memory_order_acquire);
    if (state != HA_STOPPED && (v->capture_drift_ppm != s->capture_drift ||
        v->playback_drift_ppm != s->playback_drift || v->host_time_ns != s->host_ns))
        return HA_INVALID_STATE;
    s->faults = v->faults; s->start_delay = v->start_delay_polls;
    s->stop_delay = v->stop_delay_polls; s->flush_delay = v->flush_delay_polls;
    s->capture_drift = v->capture_drift_ppm; s->playback_drift = v->playback_drift_ppm;
    s->host_ns = v->host_time_ns;
    return HA_OK;
}

ha_result hail_clock_info(ha_stream *s, uint32_t leg, uint32_t frames, ha_block_info *info)
{
    const ha_format *f = leg ? &s->info.playback_format : &s->info.capture_format;
    int32_t ppm = leg ? s->playback_drift : s->capture_drift;
    uint64_t denominator = (uint64_t)f->sample_rate * (uint32_t)(1000000 + ppm);
    /* frames <= 8192, so frames * 10^15 + remainder fits uint64_t. */
    uint64_t numerator = (uint64_t)frames * UINT64_C(1000000000000000) + s->clock_remainder[leg];
    uint64_t delta = numerator / denominator;
    if (delta > UINT64_MAX - s->clock_ns[leg]) return HA_OVERFLOW;
    memset(info, 0, sizeof(*info));
    info->struct_size = sizeof(*info); info->struct_version = HA_STRUCT_VERSION;
    info->first_frame = leg ? s->playback_position : s->capture_position;
    info->device_position = info->first_frame;
    info->clock_domain = leg ? s->info.playback_clock_domain : s->info.capture_clock_domain;
    info->clock_epoch = leg ? s->playback_epoch : s->capture_epoch;
    info->generation = leg ? s->generation : 0;
    info->host_time_ns = s->clock_ns[leg]; info->uncertainty_ns = 1;
    info->frame_time_numerator = UINT64_C(1000000000000000);
    info->frame_time_denominator = denominator;
    info->time_remainder = s->clock_remainder[leg];
    info->frames = frames; info->sample_rate = f->sample_rate;
    info->lost_frames = leg ? s->status.reference_lost_frames : s->status.capture_lost_frames;
    info->flags = HA_TIME_HOST_VALID | HA_TIME_DEVICE_VALID |
                  (leg ? s->playback_flags : s->capture_flags);
    s->clock_ns[leg] += delta;
    s->clock_remainder[leg] = numerator % denominator;
    return HA_OK;
}

static void synthetic(ha_stream *s, uint32_t frames)
{
    const ha_format *f = &s->info.capture_format;
    uint32_t i, ch;
    for (i = 0; i < frames; ++i) for (ch = 0; ch < f->channels; ++ch) {
        uint32_t phase = (uint32_t)((s->capture_position + i) & 65535u);
        int32_t sample = (int32_t)((phase + ch * 17u) & 65535u) - 32768;
        size_t index = (size_t)i * f->channels + ch;
        if (f->sample_format == HA_PCM_S16) {
            int16_t value = (int16_t)sample;
            memcpy(s->scratch + index * 2u, &value, sizeof(value));
        } else {
            float value = (float)sample / 32768.0f;
            memcpy(s->scratch + index * 4u, &value, sizeof(value));
        }
    }
}

ha_result HA_CALL ha_virtual_step(ha_stream *s, const ha_virtual_step_info *step,
                                  void *playback, uint32_t capacity, ha_block_info *out)
{
    ha_block_info capture = HA_INIT(ha_block_info), render = HA_INIT(ha_block_info);
    ha_result r;
    uint32_t state;
    if (!s || !HAIL_HEADER(step) || !HAIL_HEADER(out) || step->reserved ||
        s->context->backend->id != HA_BACKEND_VIRTUAL ||
        (step->flags & ~(HA_VIRTUAL_DISCONTINUITY_CAPTURE | HA_VIRTUAL_DISCONTINUITY_PLAYBACK |
                         HA_VIRTUAL_UNKNOWN_CAPTURE_GAP)) ||
        step->capture_frames > s->info.quantum_frames || step->playback_frames > s->info.quantum_frames ||
        (step->capture_frames && !s->capture.slots) ||
        (step->playback_frames && (!s->playback.slots || !playback))) return HA_INVALID_ARGUMENT;
    if (capacity < step->playback_frames) return HA_TOO_SMALL;
    state = atomic_load_explicit(&s->state, memory_order_acquire);
    if (state != HA_RUNNING) return HA_INVALID_STATE;
    if (step->advance_ns > UINT64_MAX - s->host_ns ||
        step->capture_frames > UINT64_MAX - s->capture_position ||
        step->playback_frames > UINT64_MAX - s->playback_position) {
        hail_fail(s, HA_OVERFLOW); return HA_OVERFLOW;
    }
    if ((step->flags & (HA_VIRTUAL_DISCONTINUITY_CAPTURE | HA_VIRTUAL_UNKNOWN_CAPTURE_GAP)) &&
        s->capture_epoch == UINT64_MAX) { hail_fail(s, HA_OVERFLOW); return HA_OVERFLOW; }
    if ((step->flags & HA_VIRTUAL_DISCONTINUITY_PLAYBACK) && s->playback_epoch == UINT64_MAX) {
        hail_fail(s, HA_OVERFLOW); return HA_OVERFLOW;
    }
    s->host_ns += step->advance_ns;
    if (step->flags & (HA_VIRTUAL_DISCONTINUITY_CAPTURE | HA_VIRTUAL_UNKNOWN_CAPTURE_GAP)) {
        ++s->capture_epoch; s->capture_flags |= HA_BLOCK_DISCONTINUITY;
        s->clock_ns[0] = s->host_ns; s->clock_remainder[0] = 0;
        if (step->flags & HA_VIRTUAL_UNKNOWN_CAPTURE_GAP)
            s->capture_flags |= HA_BLOCK_UNKNOWN_GAP | HA_BLOCK_GAP;
        hail_event(s->context, HA_EVENT_CLOCK, s, 0, HA_OK);
    }
    if (step->flags & HA_VIRTUAL_DISCONTINUITY_PLAYBACK) {
        ++s->playback_epoch; s->playback_flags |= HA_BLOCK_DISCONTINUITY;
        s->clock_ns[1] = s->host_ns; s->clock_remainder[1] = 0;
        hail_event(s->context, HA_EVENT_CLOCK, s, 0, HA_OK);
    }
    if (step->capture_frames) {
        const void *data = step->capture_data;
        if (!data) { synthetic(s, step->capture_frames); data = s->scratch; }
        r = hail_clock_info(s, 0, step->capture_frames, &capture);
        if (r != HA_OK) { hail_fail(s, r); return r; }
        r = hail_capture(s, data, step->capture_frames, &capture);
        if (r != HA_OK) { hail_fail(s, r); return r; }
    }
    if (step->playback_frames) {
        r = hail_clock_info(s, 1, step->playback_frames, &render);
        if (r != HA_OK) { hail_fail(s, r); return r; }
        r = hail_render(s, playback, step->playback_frames, &render);
        if (r != HA_OK) { hail_fail(s, r); return r; }
    }
    return HAIL_RECORD(out, render);
}

ha_result HA_CALL ha_virtual_device_set(ha_context *c, uint32_t direction,
                                        uint32_t connected, uint32_t permitted,
                                        uint64_t *id)
{
    uint32_t leg, i;
    uint64_t old;
    if (!c || !id || (direction != HA_CAPTURE && direction != HA_PLAYBACK) ||
        connected > 1 || permitted > 1 || c->backend->id != HA_BACKEND_VIRTUAL)
        return HA_INVALID_ARGUMENT;
    leg = direction == HA_CAPTURE ? 0u : 1u; old = c->default_id[leg];
    if (connected && !old) {
        ha_result r = add_device(c, direction, permitted, &c->default_id[leg]);
        if (r != HA_OK) return r;
    } else if (!connected && old) {
        for (i = 0; i < c->device_count; ++i) if (c->devices[i].info.runtime_id == old) {
            --c->device_count;
            c->devices[i] = c->devices[c->device_count]; break;
        }
        c->default_id[leg] = 0;
    } else if (old) {
        hail_device *d = hail_device_find(c, old);
        if (!d) return HA_DEVICE_UNAVAILABLE;
        d->permitted = permitted;
    }
    for (i = 0; i < c->max_streams; ++i) {
        ha_stream *s = c->streams[i];
        if (!s) continue;
        if (old && (leg ? s->info.playback_device_id : s->info.capture_device_id) == old &&
            (!connected || !permitted)) hail_fail(s, connected ? HA_PERMISSION_DENIED : HA_DEVICE_LOST);
    }
    *id = c->default_id[leg];
    hail_event(c, HA_EVENT_DEVICE, NULL, old ? old : *id, HA_OK);
    if (old != *id) hail_event(c, HA_EVENT_DEFAULT, NULL, *id, HA_OK);
    return HA_OK;
}

ha_result HA_CALL ha_virtual_device_add(ha_context *c, uint32_t direction,
                                        uint32_t permitted, uint64_t *id)
{
    ha_result r;
    if (!c || !id || (direction != HA_CAPTURE && direction != HA_PLAYBACK) ||
        permitted > 1 || c->backend->id != HA_BACKEND_VIRTUAL) return HA_INVALID_ARGUMENT;
    r = add_device(c, direction, permitted, id);
    if (r == HA_OK) hail_event(c, HA_EVENT_DEVICE, NULL, *id, HA_OK);
    return r;
}

ha_result HA_CALL ha_virtual_default_set(ha_context *c, uint32_t direction, uint64_t id)
{
    hail_device *d;
    if (!c || (direction != HA_CAPTURE && direction != HA_PLAYBACK) ||
        c->backend->id != HA_BACKEND_VIRTUAL) return HA_INVALID_ARGUMENT;
    d = hail_device_find(c, id);
    if (!d || d->info.direction != direction) return HA_DEVICE_UNAVAILABLE;
    c->default_id[direction == HA_CAPTURE ? 0 : 1] = id;
    hail_event(c, HA_EVENT_DEFAULT, NULL, id, HA_OK);
    return HA_OK;
}
