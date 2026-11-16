#include "hail_internal.h"

#if HA_ENABLE_ALSA
extern const hail_backend_ops hail_alsa_backend;
#endif
#if HA_ENABLE_PIPEWIRE
extern const hail_backend_ops hail_pipewire_backend;
#endif
#if HA_ENABLE_COREAUDIO
extern const hail_backend_ops hail_coreaudio_backend;
#endif
#if HA_ENABLE_WASAPI
extern const hail_backend_ops hail_wasapi_backend;
#endif
const hail_backend_ops *const hail_backends[] = {
#if HA_ENABLE_COREAUDIO
    &hail_coreaudio_backend,
#endif
#if HA_ENABLE_WASAPI
    &hail_wasapi_backend,
#endif
#if HA_ENABLE_PIPEWIRE
    &hail_pipewire_backend,
#endif
#if HA_ENABLE_ALSA
    &hail_alsa_backend,
#endif
#if HA_ENABLE_VIRTUAL
    &hail_virtual_backend,
#endif
    NULL
};

const hail_backend_ops *hail_backend_select(uint32_t id, uint32_t attempt)
{
    uint32_t i;
    for (i = 0; hail_backends[i]; ++i) {
        const hail_backend_ops *b = hail_backends[i];
        if (id == HA_BACKEND_AUTO ? b->id != HA_BACKEND_VIRTUAL : b->id == id) {
            if (!attempt) return b;
            --attempt;
        }
    }
    return NULL;
}

size_t hail_backend_context_bytes(uint32_t id)
{
    size_t max = 0;
    uint32_t i;
    for (i = 0; hail_backends[i]; ++i)
        if ((id == HA_BACKEND_AUTO || hail_backends[i]->id == id) && hail_backends[i]->context_bytes > max)
            max = hail_backends[i]->context_bytes;
    return hail_align(max);
}

void hail_rt_init(ha_stream *s)
{
    uint32_t i;
    atomic_init(&s->flush_requested, 0); atomic_init(&s->rt_flags, 0);
    for (i = 0; i < 2; ++i) {
        atomic_init(&s->rt[i].gate, HAIL_GATE_CLOSED);
        atomic_init(&s->rt[i].write_index, 0); atomic_init(&s->rt[i].read_index, 0);
        atomic_init(&s->rt[i].error, 0);
    }
}

int hail_rt_enter(ha_stream *s, uint32_t leg)
{
    uint32_t state;
    if (!hail_gate_enter(&s->rt[leg].gate)) return 0;
    if (atomic_load_explicit(&s->rt[0].error, memory_order_acquire) ||
        atomic_load_explicit(&s->rt[1].error, memory_order_acquire)) {
        hail_gate_leave(&s->rt[leg].gate); return 0;
    }
    state = atomic_load_explicit(&s->state, memory_order_acquire);
    if (state != HA_RUNNING) { hail_gate_leave(&s->rt[leg].gate); return 0; }
    return 1;
}

void hail_rt_leave(ha_stream *s, uint32_t leg)
{
    hail_rt_leg *rt = &s->rt[leg];
    unsigned w = atomic_load_explicit(&rt->write_index, memory_order_relaxed);
    unsigned r = atomic_load_explicit(&rt->read_index, memory_order_acquire);
    rt->current.epoch = leg ? s->playback_epoch : s->capture_epoch;
    if ((unsigned)(w - r) < 2u) {
        rt->snapshots[w & 1u] = rt->current;
        atomic_store_explicit(&rt->write_index, w + 1u, memory_order_release);
        rt->current.events = 0;
    }
    hail_gate_leave(&rt->gate);
}

int hail_rt_close(ha_stream *s, uint32_t leg)
{
    unsigned expected = HAIL_GATE_IDLE;
    return atomic_compare_exchange_strong_explicit(&s->rt[leg].gate, &expected,
        HAIL_GATE_CLOSED, memory_order_acq_rel, memory_order_relaxed) || expected == HAIL_GATE_CLOSED;
}
void hail_rt_open(ha_stream *s, uint32_t leg) { hail_gate_leave(&s->rt[leg].gate); }

void hail_rt_error(ha_stream *s, uint32_t leg, ha_result r, int64_t native)
{
    int expected = 0;
    /* Native control notifications and audio processing may both fail. Claim
     * the single diagnostic payload with one bounded CAS; INT_MIN means that
     * the winning producer has not release-published the payload yet. */
    if (atomic_compare_exchange_strong_explicit(&s->rt[leg].error, &expected, INT_MIN,
            memory_order_acquire, memory_order_relaxed)) {
        s->rt[leg].native_error = native;
        atomic_store_explicit(&s->rt[leg].error, r, memory_order_release);
    }
}

void hail_signal(ha_stream *s, uint32_t leg, uint32_t event, ha_result result)
{
    if (s->native) s->rt[leg].current.events |= 1u << event;
    else hail_event(s->context, event, s, 0, result);
}

void hail_rt_collect(ha_stream *s)
{
    uint32_t leg;
    if (!s->native) return;
    for (leg = 0; leg < 2; ++leg) {
        hail_rt_leg *rt = &s->rt[leg];
        unsigned r = atomic_load_explicit(&rt->read_index, memory_order_relaxed);
        unsigned w = atomic_load_explicit(&rt->write_index, memory_order_acquire);
        ha_result error;
        while (r != w) {
            hail_rt_snapshot v = rt->snapshots[r & 1u];
            if (leg) {
                s->status.playback_underrun_frames = v.underrun;
                s->status.reference_lost_frames = v.reference_lost; s->status.playback_epoch = v.epoch;
            } else { s->status.capture_lost_frames = v.lost; s->status.capture_epoch = v.epoch; }
            if (v.events & (1u << HA_EVENT_XRUN)) hail_event(s->context, HA_EVENT_XRUN, s, 0, HA_OK);
            if (v.events & (1u << HA_EVENT_CLOCK)) hail_event(s->context, HA_EVENT_CLOCK, s, 0, HA_OK);
            ++r;
        }
        atomic_store_explicit(&rt->read_index, r, memory_order_release);
        error = atomic_load_explicit(&rt->error, memory_order_acquire);
        if (error && error != INT_MIN) {
            s->status.diagnostic.native_code = rt->native_error;
            hail_fail(s, error);
        }
    }
    s->status.flags |= atomic_load_explicit(&s->rt_flags, memory_order_relaxed);
}

void hail_rt_final(ha_stream *s)
{
    if (!s->native) return;
    /* Both producer gates are CLOSED and native retirement has completed. */
    hail_rt_collect(s);
    s->status.capture_lost_frames = s->rt[0].current.lost;
    s->status.playback_underrun_frames = s->rt[1].current.underrun;
    s->status.reference_lost_frames = s->rt[1].current.reference_lost;
    s->status.capture_epoch = s->capture_epoch; s->status.playback_epoch = s->playback_epoch;
}

static ha_block_info native_block(ha_stream *s, uint32_t leg, uint32_t frames,
                                   uint64_t host, uint64_t position, uint64_t uncertainty, uint32_t flags)
{
    ha_block_info b = HA_INIT(ha_block_info);
    b.first_frame = leg ? s->playback_position : s->capture_position;
    b.clock_domain = leg ? s->info.playback_clock_domain : s->info.capture_clock_domain;
    b.clock_epoch = leg ? s->playback_epoch : s->capture_epoch;
    b.generation = leg ? s->render_generation : 0;
    b.host_time_ns = host; b.device_position = position; b.uncertainty_ns = uncertainty;
    b.frames = frames; b.flags = flags | (leg ? s->playback_flags : s->capture_flags);
    b.sample_rate = leg ? s->info.playback_format.sample_rate : s->info.capture_format.sample_rate;
    b.frame_time_numerator = UINT64_C(1000000000); b.frame_time_denominator = b.sample_rate;
    b.lost_frames = leg ? s->rt[1].current.reference_lost : s->rt[0].current.lost;
    return b;
}

static ha_result native_clock(ha_stream *s, uint32_t leg, ha_block_info *b, uint32_t native_flags)
{
    hail_rt_leg *rt = &s->rt[leg];
    uint64_t *epoch = leg ? &s->playback_epoch : &s->capture_epoch;
    int reset = (native_flags & (HA_BLOCK_UNKNOWN_GAP | HA_BLOCK_DISCONTINUITY)) != 0;
    if ((b->flags & HA_TIME_DEVICE_VALID) && rt->device_valid && b->device_position != rt->last_device) reset = 1;
    if ((b->flags & HA_TIME_HOST_VALID) && rt->host_valid && b->host_time_ns < rt->last_host) {
        if (b->flags & HA_TIME_ESTIMATED) { b->flags &= ~HA_TIME_HOST_VALID; hail_signal(s, leg, HA_EVENT_CLOCK, HA_OK); }
        else reset = 1;
    }
    if (reset) {
        if (*epoch == UINT64_MAX) return HA_OVERFLOW;
        b->clock_epoch = ++*epoch; b->flags |= HA_BLOCK_DISCONTINUITY;
        if (!leg) b->flags |= HA_BLOCK_UNKNOWN_GAP;
        rt->host_valid = 0; rt->device_valid = 0;
        hail_signal(s, leg, HA_EVENT_CLOCK, HA_OK);
    }
    if (b->flags & HA_TIME_HOST_VALID) { rt->host_valid = 1; rt->last_host = b->host_time_ns; }
    if (b->flags & HA_TIME_DEVICE_VALID) {
        if (b->device_position > UINT64_MAX - b->frames) return HA_OVERFLOW;
        rt->device_valid = 1; rt->last_device = b->device_position + b->frames;
    }
    return HA_OK;
}

ha_result hail_native_capture(ha_stream *s, const void *data, uint32_t frames,
                               uint64_t host, uint64_t pos, uint64_t uncertainty, uint32_t flags)
{
    ha_block_info b;
    if ((!data && frames) || frames > s->info.quantum_frames || !s->capture.slots) return HA_INVALID_ARGUMENT;
    b = native_block(s, 0, frames, host, pos, uncertainty, flags);
    ha_result r = native_clock(s, 0, &b, flags);
    if (r != HA_OK) return r;
    return hail_capture(s, data, frames, &b);
}

ha_result hail_native_capture_loss(ha_stream *s, uint32_t frames, uint32_t unknown)
{
    if (frames > UINT64_MAX - s->capture_position) return HA_OVERFLOW;
    if (unknown) {
        if (s->capture_epoch == UINT64_MAX) return HA_OVERFLOW;
        ++s->capture_epoch; s->capture_flags |= HA_BLOCK_UNKNOWN_GAP | HA_BLOCK_DISCONTINUITY;
        s->rt[0].host_valid = 0; s->rt[0].device_valid = 0;
    }
    s->capture_position += frames;
    hail_counter(s, &s->rt[0].current.lost, frames); s->capture_flags |= HA_BLOCK_GAP;
    hail_signal(s, 0, HA_EVENT_XRUN, HA_OVERFLOW);
    return HA_OK;
}

ha_result hail_native_render(ha_stream *s, void *data, uint32_t frames,
                              uint64_t host, uint64_t pos, uint64_t uncertainty, uint32_t flags)
{
    ha_block_info b;
    if ((!data && frames) || frames > s->info.quantum_frames || !s->playback.slots) return HA_INVALID_ARGUMENT;
    b = native_block(s, 1, frames, host, pos, uncertainty, flags);
    ha_result r = native_clock(s, 1, &b, flags);
    if (r != HA_OK) return r;
    return hail_render(s, data, frames, &b);
}

ha_result hail_registry_publish(ha_context *c, hail_device *next, uint32_t count,
                                 const char *capture_default, const char *playback_default)
{
    uint32_t i, j;
    uint64_t defaults[2] = {0, 0};
    if (count > c->max_devices) return HA_RESOURCE_EXHAUSTED;
    for (i = 0; i < count; ++i) {
        uint64_t id = 0;
        if (!next[i].key[0]) return HA_INVALID_ARGUMENT;
        for (j = 0; j < c->device_count; ++j)
            if (next[i].info.direction == c->devices[j].info.direction && next[i].native_id == c->devices[j].native_id &&
                !strcmp(next[i].key, c->devices[j].key))
                id = c->devices[j].info.runtime_id;
        if (!id) { if (c->next_id == UINT64_MAX) return HA_OVERFLOW; id = c->next_id++; }
        next[i].info.runtime_id = id; next[i].info.backend = c->backend->id; next[i].info.capabilities = c->backend->features;
        if (capture_default && next[i].info.direction == HA_CAPTURE && !strcmp(next[i].key, capture_default)) defaults[0] = id;
        if (playback_default && next[i].info.direction == HA_PLAYBACK && !strcmp(next[i].key, playback_default)) defaults[1] = id;
    }
    for (i = 0; i < count; ++i) {
        int found = 0;
        for (j = 0; j < c->device_count; ++j) if (next[i].info.runtime_id == c->devices[j].info.runtime_id) found = 1;
        if (!found) hail_event(c, HA_EVENT_DEVICE, NULL, next[i].info.runtime_id, HA_OK);
    }
    for (i = 0; i < c->device_count; ++i) {
        int found = 0;
        for (j = 0; j < count; ++j) if (c->devices[i].info.runtime_id == next[j].info.runtime_id) found = 1;
        if (!found) {
            for (j = 0; j < c->max_streams; ++j) {
                ha_stream *s = c->streams[j];
                if (s && (s->info.capture_device_id == c->devices[i].info.runtime_id ||
                          s->info.playback_device_id == c->devices[i].info.runtime_id)) hail_fail(s, HA_DEVICE_LOST);
            }
            hail_event(c, HA_EVENT_DEVICE, NULL, c->devices[i].info.runtime_id, HA_DEVICE_LOST);
        }
    }
    for (i = 0; i < 2; ++i) if (c->default_id[i] != defaults[i])
        hail_event(c, HA_EVENT_DEFAULT, NULL, defaults[i], HA_OK);
    memcpy(c->devices, next, count * sizeof(*next)); c->device_count = count;
    c->default_id[0] = defaults[0]; c->default_id[1] = defaults[1];
    return HA_OK;
}

ha_result hail_device_record(hail_device *d, uint32_t direction, const char *key,
                              const char *label, uint64_t native)
{
    size_t n = strlen(key), bytes;
    if (!n || n >= sizeof(d->key)) return HA_RESOURCE_EXHAUSTED;
    memset(d, 0, sizeof(*d));
    d->info.struct_size = sizeof(ha_device_info); d->info.struct_version = HA_STRUCT_VERSION;
    d->info.direction = direction; d->native_id = native; d->permitted = 1;
    memcpy(d->key, key, n + 1u);
    bytes = strlen(label); if (bytes >= sizeof(d->info.name)) bytes = sizeof(d->info.name) - 1u;
    /* Do not leave a partial UTF-8 sequence at a truncated display-name end. */
    while (bytes && ((unsigned char)label[bytes] & 0xc0u) == 0x80u) --bytes;
    memcpy(d->info.name, label, bytes); d->info.name_bytes = (uint32_t)bytes;
    return HA_OK;
}

void hail_device_persistent(hail_device *d)
{
    size_t n = strlen(d->key);
    if (n < sizeof(d->info.persistent_key)) {
        memcpy(d->info.persistent_key, d->key, n + 1u);
        d->info.persistent_key_bytes = (uint32_t)n; d->info.identity_flags = HA_IDENTITY_OS_SCOPED;
    }
}

void hail_registry_remove(ha_context *c, const char *key, uint64_t native_id, uint32_t direction)
{
    uint32_t i = 0;
    while (i < c->device_count) {
        hail_device *d = &c->devices[i];
        if ((d->info.direction & direction) && (key ? !strcmp(d->key, key) : native_id && d->native_id == native_id)) {
            uint64_t id = d->info.runtime_id; uint32_t j;
            for (j = 0; j < c->max_streams; ++j) {
                ha_stream *s = c->streams[j];
                if (s && (s->info.capture_device_id == id || s->info.playback_device_id == id)) hail_fail(s, HA_DEVICE_LOST);
            }
            for (j = 0; j < 2; ++j) if (c->default_id[j] == id) c->default_id[j] = 0;
            --c->device_count; c->devices[i] = c->devices[c->device_count];
            hail_event(c, HA_EVENT_DEVICE, NULL, id, HA_DEVICE_LOST);
        } else ++i;
    }
}
