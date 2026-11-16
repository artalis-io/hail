#include "hail_internal.h"

static uint32_t features(void)
{
    uint32_t f = 0, i;
    for (i = 0; hail_backends[i]; ++i) f |= hail_backends[i]->features;
    return f;
}

ha_result HA_CALL ha_get_version(ha_version_info *out)
{
    ha_version_info v = HA_INIT(ha_version_info);
    v.major = HA_VERSION_MAJOR; v.minor = HA_VERSION_MINOR; v.patch = HA_VERSION_PATCH;
    v.features = features(); /* ABI major zero deliberately remains unstable. */
    return HAIL_RECORD(out, v);
}

const char *HA_CALL ha_result_string(ha_result r)
{
    switch (r) {
    case HA_OK: return "success";
    case HA_PENDING: return "pending";
    case HA_WOULD_BLOCK: return "would block";
    case HA_BUSY: return "busy";
    case HA_TOO_SMALL: return "output too small";
    case HA_INVALID_ARGUMENT: return "invalid argument";
    case HA_INVALID_STATE: return "invalid state";
    case HA_UNSUPPORTED: return "unsupported operation";
    case HA_FORMAT_UNSUPPORTED: return "unsupported format";
    case HA_PERMISSION_DENIED: return "permission denied";
    case HA_DEVICE_UNAVAILABLE: return "device unavailable";
    case HA_DEVICE_LOST: return "device lost";
    case HA_BACKEND_INIT_FAILED: return "backend initialization failed";
    case HA_RESOURCE_EXHAUSTED: return "resource exhausted";
    case HA_STREAM_FAILED: return "stream failed";
    case HA_STALE_GENERATION: return "stale playback generation";
    case HA_CANCELLED: return "cancelled";
    case HA_OVERFLOW: return "overflow";
    default: return "unknown result";
    }
}

static ha_result diagnostic(ha_diagnostic *out, uint32_t backend,
                             uint32_t operation, ha_result result)
{
    ha_diagnostic d = HA_INIT(ha_diagnostic);
    if (!out) return result;
    if (!HAIL_HEADER(out)) return HA_INVALID_ARGUMENT;
    d.backend = backend; d.operation = operation; d.result = result;
    (void)HAIL_RECORD(out, d);
    return result;
}

static ha_result context_config(const ha_context_config *cfg, ha_context_config *v,
                                  size_t *bytes)
{
    size_t array;
    if (!HAIL_HEADER(cfg)) return HA_INVALID_ARGUMENT;
    *v = *cfg;
    if (v->diagnostic && !HAIL_HEADER(v->diagnostic)) return HA_INVALID_ARGUMENT;
    if (!v->max_devices) v->max_devices = HA_MAX_DEVICES;
    if (!v->max_streams) v->max_streams = HA_MAX_STREAMS;
    if (!v->event_capacity) v->event_capacity = 64;
    if (!v->memory_limit) v->memory_limit = UINT64_C(64) * 1024 * 1024;
    if (v->max_devices > HA_MAX_DEVICES ||
        v->max_streams > HA_MAX_STREAMS || v->event_capacity > HA_MAX_EVENTS)
        return HA_INVALID_ARGUMENT;
    if (v->allocator.allocate || v->allocator.deallocate || v->allocator.user) {
        if (!HAIL_HEADER(&v->allocator) || !v->allocator.allocate || !v->allocator.deallocate)
            return HA_INVALID_ARGUMENT;
    } else {
#if HA_FREESTANDING
        return HA_INVALID_ARGUMENT;
#else
        v->allocator.struct_size = sizeof(ha_allocator);
        v->allocator.struct_version = HA_STRUCT_VERSION;
        v->allocator.allocate = hail_host_allocate;
        v->allocator.deallocate = hail_host_deallocate;
#endif
    }
    *bytes = hail_align(sizeof(ha_context));
    if (!hail_mul(v->max_devices, sizeof(hail_device), &array) ||
        !hail_add(bytes, hail_align(array)) ||
        !hail_mul(v->max_streams, sizeof(ha_stream *), &array) ||
        !hail_add(bytes, hail_align(array)) ||
        !hail_mul(v->event_capacity, sizeof(ha_event), &array) ||
        !hail_add(bytes, hail_align(array)) ||
        !hail_add(bytes, hail_backend_context_bytes(v->backend))) return HA_OVERFLOW;
    if ((uint64_t)*bytes > v->memory_limit) return HA_RESOURCE_EXHAUSTED;
    return HA_OK;
}

ha_result HA_CALL ha_context_plan(const ha_context_config *cfg, ha_memory_plan *out)
{
    ha_context_config v;
    ha_memory_plan p = HA_INIT(ha_memory_plan);
    size_t bytes;
    ha_result r;
    if (!HAIL_HEADER(out)) return HA_INVALID_ARGUMENT;
    r = context_config(cfg, &v, &bytes);
    if (r != HA_OK) return r;
    p.bytes = bytes; p.alignment = HAIL_ALIGNMENT; p.allocations = 1;
    return HAIL_RECORD(out, p);
}

ha_result HA_CALL ha_context_create(const ha_context_config *cfg, ha_context **out)
{
    ha_context_config v;
    const hail_backend_ops *backend = NULL;
    ha_context *c;
    size_t bytes;
    ha_result r;
    if (!out) return HA_INVALID_ARGUMENT;
    *out = NULL;
    r = context_config(cfg, &v, &bytes);
    if (r != HA_OK) return r;
    backend = hail_backend_select(v.backend, 0);
    if (!backend) return diagnostic(v.diagnostic, v.backend, 1, HA_UNSUPPORTED);
    c = v.allocator.allocate(v.allocator.user, bytes, HAIL_ALIGNMENT);
    if (!c) return diagnostic(v.diagnostic, v.backend, 1, HA_RESOURCE_EXHAUSTED);
    if ((uintptr_t)c % HAIL_ALIGNMENT) {
        v.allocator.deallocate(v.allocator.user, c, bytes, HAIL_ALIGNMENT);
        return diagnostic(v.diagnostic, v.backend, 1, HA_INVALID_ARGUMENT);
    }
    memset(c, 0, bytes);
    c->allocator = v.allocator; c->backend = backend;
    c->allocation_bytes = bytes; c->allocated = bytes; c->limit = v.memory_limit;
    c->max_devices = v.max_devices; c->max_streams = v.max_streams;
    c->event_capacity = v.event_capacity;
    c->devices = (hail_device *)(void *)((unsigned char *)c + hail_align(sizeof(*c)));
    c->streams = (ha_stream **)(void *)((unsigned char *)c->devices +
                                      hail_align(v.max_devices * sizeof(hail_device)));
    c->events = (ha_event *)(void *)((unsigned char *)c->streams +
                                    hail_align(v.max_streams * sizeof(ha_stream *)));
    c->next_id = 1;
    atomic_init(&c->registry_dirty, 0);
    c->backend_state = (unsigned char *)c->events + hail_align(v.event_capacity * sizeof(ha_event));
    {
        uint32_t attempt = 0;
        for (;;) {
            c->backend = backend;
            r = backend->context_init(c);
            if (r == HA_OK) break;
            if (backend->context_free && backend->context_free(c) != HA_OK) {
                /* Failed-init adapters must retire their unpublished users.
                 * If a broken native API cannot prove that, quarantine this
                 * bounded allocation rather than freeing callback storage. */
                return diagnostic(v.diagnostic, v.backend, 1, HA_BACKEND_INIT_FAILED);
            }
            if (v.backend != HA_BACKEND_AUTO || (r != HA_BACKEND_INIT_FAILED && r != HA_UNSUPPORTED)) break;
            backend = hail_backend_select(v.backend, ++attempt);
            if (!backend) break;
            c->device_count = 0; c->default_id[0] = 0; c->default_id[1] = 0; c->next_id = 1;
            memset(c->backend_state, 0, hail_backend_context_bytes(v.backend));
        }
    }
    if (r != HA_OK) {
        v.allocator.deallocate(v.allocator.user, c, bytes, HAIL_ALIGNMENT);
        return diagnostic(v.diagnostic, v.backend, 1, r);
    }
    *out = c;
    return diagnostic(v.diagnostic, v.backend, 1, HA_OK);
}

ha_result HA_CALL ha_context_refresh(ha_context *c)
{
    ha_result r;
    if (!c) return HA_INVALID_ARGUMENT;
    if (c->closing) return HA_INVALID_STATE;
    if (c->backend->context_poll) c->backend->context_poll(c);
    r = c->backend->refresh ? c->backend->refresh(c) : HA_OK;
    if (r != HA_OK) return r;
    c->flags &= ~HA_EVENT_RESCAN_REQUIRED;
    return HA_OK; /* Driver publishes authoritative registry changes on control. */
}

ha_result HA_CALL ha_context_get_status(ha_context *c, ha_context_status *out)
{
    ha_context_status v = HA_INIT(ha_context_status);
    if (!c) return HA_INVALID_ARGUMENT;
    v.allocated_bytes = c->allocated; v.memory_limit = c->limit;
    v.lost_events = c->lost_events; v.streams = c->stream_count; v.flags = c->flags;
    v.backend = c->backend->id; v.features = c->backend->features;
    return HAIL_RECORD(out, v);
}

ha_result HA_CALL ha_context_enumerate(ha_context *c, ha_device_info *items,
                                       uint32_t capacity, uint32_t *count)
{
    uint32_t needed, i, n = 0;
    if (!c || !count || (capacity && !items)) return HA_INVALID_ARGUMENT;
    if (c->closing) return HA_INVALID_STATE;
    needed = c->device_count;
    *count = needed;
    if (capacity < needed) return HA_TOO_SMALL;
    for (i = 0; i < needed; ++i) if (!HAIL_HEADER(&items[i])) return HA_INVALID_ARGUMENT;
    for (i = 0; i < needed; ++i) {
        ha_device_info v = c->devices[i].info;
        v.is_default = (v.runtime_id == c->default_id[0] || v.runtime_id == c->default_id[1]);
        (void)HAIL_RECORD(&items[n], v); ++n;
    }
    return HA_OK;
}

hail_device *hail_device_find(ha_context *c, uint64_t id)
{
    uint32_t i;
    for (i = 0; i < c->device_count; ++i) if (c->devices[i].info.runtime_id == id) return &c->devices[i];
    return NULL;
}

static ha_result select_device(ha_context *c, const ha_device_selector *d,
                                uint32_t leg, uint64_t *id)
{
    hail_device *device;
    if (!HAIL_HEADER(d)) return HA_INVALID_ARGUMENT;
    if (d->role != HA_ROLE_DEFAULT) return HA_UNSUPPORTED;
    if (d->reserved) return HA_INVALID_ARGUMENT;
    if (d->kind == HA_SELECT_PERSISTENT_KEY) {
        uint32_t i;
        if (!d->key_bytes || d->key_bytes >= HA_DEVICE_KEY_CAPACITY || d->key[d->key_bytes] || memchr(d->key, 0, d->key_bytes)) return HA_INVALID_ARGUMENT;
        for (i = 0; i < c->device_count; ++i) {
            const ha_device_info *info = &c->devices[i].info;
            if (info->direction == (leg ? HA_PLAYBACK : HA_CAPTURE) && info->identity_flags &&
                info->persistent_key_bytes == d->key_bytes && !memcmp(info->persistent_key, d->key, d->key_bytes)) {
                if (!c->devices[i].permitted) return HA_PERMISSION_DENIED;
                *id = info->runtime_id; return HA_OK;
            }
        }
        return HA_DEVICE_UNAVAILABLE;
    }
    if (d->kind != HA_SELECT_DEFAULT && d->kind != HA_SELECT_RUNTIME_ID)
        return HA_UNSUPPORTED;
    *id = d->kind == HA_SELECT_DEFAULT ? c->default_id[leg] : d->runtime_id;
    device = hail_device_find(c, *id);
    if (!device || !(device->info.direction & (leg ? HA_PLAYBACK : HA_CAPTURE))) return HA_DEVICE_UNAVAILABLE;
    if (!device->permitted) return HA_PERMISSION_DENIED;
    return HA_OK;
}

ha_result HA_CALL ha_device_probe(ha_context *c, const ha_device_selector *d,
                                  uint32_t direction, const ha_format *f,
                                  ha_probe_info *out)
{
    ha_probe_info v = HA_INIT(ha_probe_info);
    uint32_t bpf;
    uint64_t id;
    ha_result r;
    if (!c || !HAIL_HEADER(out) || (direction != HA_CAPTURE && direction != HA_PLAYBACK))
        return HA_INVALID_ARGUMENT;
    if (c->closing) return HA_INVALID_STATE;
    r = select_device(c, d, direction == HA_CAPTURE ? 0u : 1u, &id);
    if (r != HA_OK) return r;
    r = hail_format(f, &bpf);
    if (r != HA_OK) return r;
    r = c->backend->probe(c, id, f, HA_EXACT_APPLICATION, &v);
    if (r != HA_OK) return r;
    return HAIL_RECORD(out, v);
}

static int slots_valid(uint32_t n) { return n && n <= HA_MAX_QUEUE_SLOTS && !(n & (n - 1u)); }

static ha_result stream_config(ha_context *c, const ha_stream_config *cfg,
                                 ha_stream_info *info, size_t *bytes)
{
    uint32_t bpf[2] = {0, 0}, i;
    size_t ring_bytes, stride;
    ha_result r;
    if (!c || !HAIL_HEADER(cfg)) return HA_INVALID_ARGUMENT;
    if (c->closing) return HA_INVALID_STATE;
    if (cfg->diagnostic && !HAIL_HEADER(cfg->diagnostic)) return HA_INVALID_ARGUMENT;
    if (!cfg->directions || (cfg->directions & ~(HA_CAPTURE | HA_PLAYBACK)) ||
        cfg->negotiation > HA_EXACT_APPLICATION) return HA_INVALID_ARGUMENT;
    memset(info, 0, sizeof(*info));
    info->struct_size = sizeof(*info); info->struct_version = HA_STRUCT_VERSION;
    info->directions = cfg->directions;
    info->quantum_frames = cfg->quantum_frames ? cfg->quantum_frames : 256u;
    if (info->quantum_frames > HA_MAX_QUANTUM) return HA_INVALID_ARGUMENT;
    info->capture_slots = (cfg->directions & HA_CAPTURE) ?
        (cfg->capture_slots ? cfg->capture_slots : 8u) : 0;
    info->playback_slots = (cfg->directions & HA_PLAYBACK) ?
        (cfg->playback_slots ? cfg->playback_slots : 8u) : 0;
    info->reference_slots = cfg->reference_slots;
    if ((info->capture_slots && !slots_valid(info->capture_slots)) ||
        (info->playback_slots && !slots_valid(info->playback_slots)) ||
        (info->reference_slots && (!info->playback_slots || !slots_valid(info->reference_slots))) ||
        (!(cfg->directions & HA_CAPTURE) && cfg->capture_slots) ||
        (!(cfg->directions & HA_PLAYBACK) && cfg->playback_slots)) return HA_INVALID_ARGUMENT;
    for (i = 0; i < 2; ++i) if (cfg->directions & (i ? HA_PLAYBACK : HA_CAPTURE)) {
        const ha_format *f = i ? &cfg->playback_format : &cfg->capture_format;
        const ha_device_selector *d = i ? &cfg->playback_device : &cfg->capture_device;
        uint64_t *id = i ? &info->playback_device_id : &info->capture_device_id;
        r = select_device(c, d, i, id); if (r != HA_OK) return r;
        r = hail_format(f, &bpf[i]); if (r != HA_OK) return r;
        {
            ha_probe_info *negotiated = i ? &info->playback_negotiated : &info->capture_negotiated;
            negotiated->struct_size = sizeof(*negotiated); negotiated->struct_version = HA_STRUCT_VERSION;
            r = c->backend->probe(c, *id, f, cfg->negotiation, negotiated);
            if (r != HA_OK) return r;
        }
        if (i) info->playback_format = *f; else info->capture_format = *f;
    }
    info->features = c->backend->features;
    info->conversion = info->capture_slots ? info->capture_negotiated.conversion : info->playback_negotiated.conversion;
    if (info->capture_slots && info->playback_slots && info->capture_negotiated.conversion != info->playback_negotiated.conversion)
        info->conversion = HA_CONVERSION_UNKNOWN;
    info->clock_relationship = HA_CLOCK_UNKNOWN;
    *bytes = hail_align(sizeof(ha_stream));
    for (i = 0; i < 3; ++i) {
        uint32_t n = i == 0 ? info->capture_slots : (i == 1 ? info->playback_slots : info->reference_slots);
        if (!n) continue;
        if (!hail_mul(info->quantum_frames, bpf[i == 0 ? 0 : 1], &stride) ||
            !hail_add(&stride, sizeof(hail_slot)) || !(stride = hail_align(stride)) ||
            !hail_mul(n, stride, &ring_bytes) || !hail_add(bytes, ring_bytes)) return HA_OVERFLOW;
    }
    if (info->capture_slots) {
        if (!hail_mul(info->quantum_frames, bpf[0], &ring_bytes) ||
            !hail_add(bytes, hail_align(ring_bytes))) return HA_OVERFLOW;
    }
    if (!hail_add(bytes, hail_align(c->backend->stream_bytes))) return HA_OVERFLOW;
    info->allocated_bytes = *bytes;
    return HA_OK;
}

ha_result HA_CALL ha_stream_plan(ha_context *c, const ha_stream_config *cfg,
                                 ha_memory_plan *out)
{
    ha_stream_info info;
    ha_memory_plan p = HA_INIT(ha_memory_plan);
    size_t bytes;
    ha_result r;
    if (!HAIL_HEADER(out)) return HA_INVALID_ARGUMENT;
    r = stream_config(c, cfg, &info, &bytes); if (r != HA_OK) return r;
    p.bytes = bytes; p.alignment = HAIL_ALIGNMENT; p.allocations = 1;
    return HAIL_RECORD(out, p);
}

static void ring_init(hail_ring *r, unsigned char **storage, uint32_t slots,
                        uint32_t q, uint32_t bpf)
{
    r->storage = *storage; r->slots = slots; r->quantum = q; r->bytes_per_frame = bpf;
    r->stride = (uint32_t)hail_align(sizeof(hail_slot) + (size_t)q * bpf);
    atomic_init(&r->read_index, 0); atomic_init(&r->write_index, 0);
    *storage += (size_t)slots * r->stride;
}

ha_result HA_CALL ha_stream_open(ha_context *c, const ha_stream_config *cfg, ha_stream **out)
{
    ha_stream_info info;
    ha_stream *s;
    size_t bytes;
    uint32_t i, bpf_c = 0, bpf_p = 0;
    unsigned char *storage;
    ha_result r;
    if (!out) return HA_INVALID_ARGUMENT;
    *out = NULL;
    r = stream_config(c, cfg, &info, &bytes);
    if (r != HA_OK) {
        if (HAIL_HEADER(cfg)) return diagnostic(cfg->diagnostic, c ? c->backend->id : 0, 2, r);
        return r;
    }
    if (c->stream_count == c->max_streams || bytes > c->limit - c->allocated)
        return diagnostic(cfg->diagnostic, c->backend->id, 2, HA_RESOURCE_EXHAUSTED);
    if (c->next_id > UINT64_MAX - 3u)
        return diagnostic(cfg->diagnostic, c->backend->id, 2, HA_OVERFLOW);
    s = c->allocator.allocate(c->allocator.user, bytes, HAIL_ALIGNMENT);
    if (!s) return diagnostic(cfg->diagnostic, c->backend->id, 2, HA_RESOURCE_EXHAUSTED);
    if ((uintptr_t)s % HAIL_ALIGNMENT) {
        c->allocator.deallocate(c->allocator.user, s, bytes, HAIL_ALIGNMENT);
        return diagnostic(cfg->diagnostic, c->backend->id, 2, HA_INVALID_ARGUMENT);
    }
    memset(s, 0, bytes);
    s->context = c; s->info = info; s->allocation_bytes = bytes;
    s->id = c->next_id++; s->generation = 1;
    s->render_generation = 1; s->native = c->backend->id != HA_BACKEND_VIRTUAL;
    s->negotiation = cfg->negotiation;
    s->info.capture_clock_domain = info.capture_slots ? c->next_id++ : 0;
    s->info.playback_clock_domain = info.playback_slots ? c->next_id++ : 0;
    s->status.struct_size = sizeof(s->status); s->status.struct_version = HA_STRUCT_VERSION;
    s->status.diagnostic.struct_size = sizeof(ha_diagnostic);
    s->status.diagnostic.struct_version = HA_STRUCT_VERSION;
    s->status.flush_result = HA_OK;
    atomic_init(&s->state, HA_STOPPED);
    atomic_init(&s->read_gate, HAIL_GATE_IDLE);
    atomic_init(&s->write_gate, HAIL_GATE_IDLE);
    atomic_init(&s->reference_gate, HAIL_GATE_IDLE);
    hail_rt_init(s);
    if (info.capture_slots) (void)hail_format(&info.capture_format, &bpf_c);
    if (info.playback_slots) (void)hail_format(&info.playback_format, &bpf_p);
    storage = (unsigned char *)s + hail_align(sizeof(*s));
    ring_init(&s->capture, &storage, info.capture_slots, info.quantum_frames, bpf_c);
    ring_init(&s->playback, &storage, info.playback_slots, info.quantum_frames, bpf_p);
    ring_init(&s->reference, &storage, info.reference_slots, info.quantum_frames, bpf_p);
    s->scratch = storage;
    s->backend_state = storage + (info.capture_slots ? hail_align((size_t)info.quantum_frames * bpf_c) : 0);
    r = c->backend->open(s);
    if (r != HA_OK) {
        c->allocator.deallocate(c->allocator.user, s, bytes, HAIL_ALIGNMENT);
        return diagnostic(cfg->diagnostic, c->backend->id, 2, r);
    }
    for (i = 0; i < c->max_streams; ++i) if (!c->streams[i]) { c->streams[i] = s; break; }
    c->stream_count++; c->allocated += bytes;
    *out = s;
    return diagnostic(cfg->diagnostic, c->backend->id, 2, HA_OK);
}

void hail_event(ha_context *c, uint32_t type, ha_stream *s, uint64_t device, ha_result result)
{
    ha_event v = HA_INIT(ha_event);
    if (c->event_count == c->event_capacity) {
        if (c->lost_events != UINT64_MAX) ++c->lost_events;
        c->flags |= HA_EVENT_RESCAN_REQUIRED;
        return;
    }
    v.type = type; v.device_id = device; v.result = result;
    if (s) {
        v.stream_id = s->id; v.state = atomic_load_explicit(&s->state, memory_order_relaxed);
        v.generation = s->generation;
    }
    c->events[(c->event_read + c->event_count) % c->event_capacity] = v;
    ++c->event_count;
}

void hail_fail(ha_stream *s, ha_result result)
{
    uint32_t state = atomic_load_explicit(&s->state, memory_order_relaxed);
    if (state == HA_CLOSING || state == HA_FAILED) return;
    s->status.terminal_result = result;
    s->status.diagnostic.backend = s->context->backend->id;
    s->status.diagnostic.result = result;
    if (s->flush_pending) {
        s->status.flush_completed_generation = s->generation;
        s->status.flush_result = result; s->flush_pending = 0;
    }
    atomic_store_explicit(&s->state, HA_FAILED, memory_order_release);
    hail_event(s->context, HA_EVENT_STATE, s, 0, result);
}

ha_result HA_CALL ha_stream_get_info(ha_stream *s, ha_stream_info *out)
{
    return s ? HAIL_RECORD(out, s->info) : HA_INVALID_ARGUMENT;
}

ha_result HA_CALL ha_stream_get_status(ha_stream *s, ha_stream_status *out)
{
    ha_stream_status v;
    if (!s) return HA_INVALID_ARGUMENT;
    hail_rt_collect(s);
    v = s->status;
    v.state = atomic_load_explicit(&s->state, memory_order_acquire);
    v.playback_generation = s->generation;
    if (!s->native) { v.capture_epoch = s->capture_epoch; v.playback_epoch = s->playback_epoch; }
    return HAIL_RECORD(out, v);
}

ha_result HA_CALL ha_stream_start(ha_stream *s)
{
    if (!s) return HA_INVALID_ARGUMENT;
    if (atomic_load_explicit(&s->state, memory_order_acquire) != HA_STOPPED) return HA_INVALID_STATE;
    if (s->flush_pending) return HA_BUSY;
    hail_rt_collect(s);
    if (atomic_load_explicit(&s->state, memory_order_acquire) != HA_STOPPED) return HA_INVALID_STATE;
    if (s->capture_epoch == UINT64_MAX || s->playback_epoch == UINT64_MAX) return HA_OVERFLOW;
    ++s->capture_epoch; ++s->playback_epoch;
    s->capture_position = 0; s->playback_position = 0;
    s->render_generation = s->generation;
    s->rt[0].host_valid = 0; s->rt[0].device_valid = 0;
    s->rt[1].host_valid = 0; s->rt[1].device_valid = 0;
    s->status.capture_epoch = s->capture_epoch; s->status.playback_epoch = s->playback_epoch;
    s->capture_flags |= HA_BLOCK_DISCONTINUITY;
    s->playback_flags |= HA_BLOCK_DISCONTINUITY;
    s->control_ticks = 0;
    atomic_store_explicit(&s->state, HA_STARTING, memory_order_release);
    return HA_PENDING;
}

static void cancel_flush(ha_stream *s)
{
    if (s->flush_pending) {
        s->status.flush_completed_generation = s->generation;
        s->status.flush_result = HA_CANCELLED; s->flush_pending = 0;
        atomic_store_explicit(&s->flush_requested, 0, memory_order_release);
        hail_event(s->context, HA_EVENT_FLUSH, s, 0, HA_CANCELLED);
    }
}

ha_result HA_CALL ha_stream_stop(ha_stream *s)
{
    uint32_t state;
    if (!s) return HA_INVALID_ARGUMENT;
    state = atomic_load_explicit(&s->state, memory_order_acquire);
    if (state == HA_STOPPED) return HA_OK;
    if (state == HA_STOPPING) return HA_PENDING;
    if (state != HA_RUNNING && state != HA_STARTING) return HA_INVALID_STATE;
    cancel_flush(s); s->control_ticks = 0;
    atomic_store_explicit(&s->state, HA_STOPPING, memory_order_release);
    return HA_PENDING;
}

ha_result HA_CALL ha_stream_close_begin(ha_stream *s)
{
    if (!s) return HA_INVALID_ARGUMENT;
    if (atomic_load_explicit(&s->state, memory_order_acquire) == HA_CLOSING) return HA_PENDING;
    cancel_flush(s); s->control_ticks = 0;
    atomic_store_explicit(&s->state, HA_CLOSING, memory_order_release);
    return HA_PENDING;
}

static int close_gate(atomic_uint *gate)
{
    unsigned int expected = HAIL_GATE_IDLE;
    return atomic_compare_exchange_strong_explicit(gate, &expected, HAIL_GATE_CLOSED,
              memory_order_acq_rel, memory_order_relaxed) || expected == HAIL_GATE_CLOSED;
}

static int close_gates(ha_stream *s)
{
    /* Leave any already-claimed gate closed while another admitted role exits. */
    int a = close_gate(&s->read_gate), b = close_gate(&s->write_gate);
    int c = close_gate(&s->reference_gate);
    return a && b && c;
}

static void progress(ha_stream *s)
{
    ha_result r;
    uint32_t state;
    hail_rt_collect(s);
    state = atomic_load_explicit(&s->state, memory_order_acquire);
    if (state == HA_STARTING) {
        r = s->context->backend->start(s);
        if (r == HA_PENDING) return;
        if (r != HA_OK) { hail_fail(s, r); return; }
        atomic_store_explicit(&s->state, HA_RUNNING, memory_order_release);
        if (s->native) { hail_rt_open(s, 0); hail_rt_open(s, 1); }
        hail_event(s->context, HA_EVENT_STATE, s, 0, HA_OK);
    } else if (state == HA_STOPPING) {
        if (s->native) { int a = hail_rt_close(s, 0), b = hail_rt_close(s, 1); if (!a || !b) return; }
        hail_rt_collect(s);
        if (atomic_load_explicit(&s->state, memory_order_acquire) != HA_STOPPING) return;
        r = s->context->backend->stop(s);
        if (r == HA_PENDING) return;
        if (r != HA_OK) { hail_fail(s, r); return; }
        if (!close_gates(s)) return;
        hail_rt_final(s);
        if (atomic_load_explicit(&s->state, memory_order_acquire) != HA_STOPPING) return;
        if (s->generation == UINT64_MAX) { hail_fail(s, HA_OVERFLOW); return; }
        ++s->generation;
        hail_ring_reset(&s->capture); hail_ring_reset(&s->playback); hail_ring_reset(&s->reference);
        atomic_store_explicit(&s->state, HA_STOPPED, memory_order_release);
        hail_gate_leave(&s->read_gate); hail_gate_leave(&s->write_gate); hail_gate_leave(&s->reference_gate);
        hail_event(s->context, HA_EVENT_STATE, s, 0, HA_OK);
    } else if (state == HA_CLOSING) {
        if (s->native) { int a = hail_rt_close(s, 0), b = hail_rt_close(s, 1); if (!a || !b) return; }
        r = s->context->backend->retire(s);
        if (r == HA_OK && close_gates(s)) { hail_rt_final(s); s->retire_ready = 1; }
    } else if (state == HA_FAILED) {
        /* An error revokes admission; retirement still requires explicit proof. */
        if (!s->native || (hail_rt_close(s, 0) && hail_rt_close(s, 1))) (void)s->context->backend->retire(s);
    }
    if (s->flush_pending) {
        if (s->native && !hail_rt_close(s, 1)) return;
        hail_rt_collect(s);
        if (!s->flush_pending) return;
        r = s->context->backend->flush(s);
        if (r == HA_PENDING) return;
        s->status.flush_completed_generation = s->generation;
        s->status.flush_result = r; s->flush_pending = 0;
        if (r != HA_OK) { hail_fail(s, r); return; }
        if (s->native) {
            s->playback.offset = 0;
            atomic_store_explicit(&s->playback.read_index, s->flush_cutoff, memory_order_release);
            s->render_generation = s->generation;
            atomic_store_explicit(&s->flush_requested, 0, memory_order_release);
            {
                /* Native render servicing pauses/reset during flush. Even a
                 * best-effort flush starts a new render-selection epoch; the
                 * capture producer and clock epoch are independent. */
                if (s->playback_epoch == UINT64_MAX) { hail_fail(s, HA_OVERFLOW); return; }
                ++s->playback_epoch; s->playback_position = 0; s->playback_flags |= HA_BLOCK_DISCONTINUITY;
                s->status.playback_epoch = s->playback_epoch;
                s->rt[1].host_valid = 0; s->rt[1].device_valid = 0;
            }
            if (state == HA_RUNNING) hail_rt_open(s, 1);
        }
        hail_gate_leave(&s->write_gate);
        hail_event(s->context, HA_EVENT_FLUSH, s, 0, HA_OK);
    }
}

ha_result HA_CALL ha_context_poll(ha_context *c, ha_event *events,
                                  uint32_t capacity, uint32_t *count)
{
    uint32_t i, n;
    if (!c || !count || (capacity && !events) || capacity > HA_MAX_EVENTS) return HA_INVALID_ARGUMENT;
    if (c->closing) return HA_INVALID_STATE;
    *count = 0;
    for (i = 0; i < capacity; ++i) if (!HAIL_HEADER(&events[i])) return HA_INVALID_ARGUMENT;
    if (c->backend->context_poll) c->backend->context_poll(c);
    if (atomic_exchange_explicit(&c->registry_dirty, 0, memory_order_acquire)) {
        c->flags |= HA_EVENT_RESCAN_REQUIRED;
        hail_event(c, HA_EVENT_DEVICE, NULL, 0, HA_OK);
    }
    for (i = 0; i < c->max_streams; ++i) if (c->streams[i]) progress(c->streams[i]);
    n = c->event_count < capacity ? c->event_count : capacity;
    for (i = 0; i < n; ++i) {
        (void)HAIL_RECORD(&events[i], c->events[c->event_read]);
        c->event_read = (c->event_read + 1u) % c->event_capacity;
    }
    c->event_count -= n; *count = n;
    return HA_OK;
}

ha_result HA_CALL ha_stream_close_finish(ha_stream **ptr)
{
    ha_stream *s;
    ha_context *c;
    uint32_t i;
    if (!ptr) return HA_INVALID_ARGUMENT;
    s = *ptr; if (!s) return HA_OK;
    if (atomic_load_explicit(&s->state, memory_order_acquire) != HA_CLOSING) return HA_INVALID_STATE;
    if (!s->retire_ready) return HA_PENDING;
    c = s->context;
    for (i = 0; i < c->max_streams; ++i) if (c->streams[i] == s) { c->streams[i] = NULL; break; }
    c->allocated -= s->allocation_bytes; --c->stream_count;
    if (c->backend->stream_free) c->backend->stream_free(s);
    c->allocator.deallocate(c->allocator.user, s, s->allocation_bytes, HAIL_ALIGNMENT);
    *ptr = NULL;
    return HA_OK;
}

ha_result HA_CALL ha_context_destroy(ha_context **ptr)
{
    ha_context *c;
    ha_allocator a;
    if (!ptr) return HA_INVALID_ARGUMENT;
    c = *ptr; if (!c) return HA_OK;
    if (c->stream_count) return HA_BUSY;
    c->closing = 1;
    if (c->backend->context_free) {
        ha_result r = c->backend->context_free(c);
        if (r != HA_OK) return r;
    }
    a = c->allocator;
    a.deallocate(a.user, c, c->allocation_bytes, HAIL_ALIGNMENT);
    *ptr = NULL;
    return HA_OK;
}

static ha_result read_pcm(ha_stream *s, hail_ring *ring, atomic_uint *gate,
                            void *dst, uint32_t capacity, uint32_t *frames, ha_block_info *out)
{
    uint32_t state, n;
    ha_block_info info = HA_INIT(ha_block_info);
    if (!frames) return HA_INVALID_ARGUMENT;
    *frames = 0;
    if (!HAIL_HEADER(out) || capacity > HA_MAX_QUANTUM || (capacity && !dst)) return HA_INVALID_ARGUMENT;
    if (!ring->slots) return HA_UNSUPPORTED;
    if (!hail_gate_enter(gate)) return HA_BUSY;
    state = atomic_load_explicit(&s->state, memory_order_acquire);
    if (state != HA_RUNNING && state != HA_STARTING && state != HA_STOPPING) {
        hail_gate_leave(gate); return HA_INVALID_STATE;
    }
    n = hail_ring_read(ring, dst, capacity, &info);
    hail_gate_leave(gate);
    *frames = n; (void)HAIL_RECORD(out, info);
    return n || !capacity ? HA_OK : HA_WOULD_BLOCK;
}

ha_result HA_CALL ha_stream_read(ha_stream *s, void *dst, uint32_t capacity,
                                 uint32_t *frames, ha_block_info *out)
{
    if (!s) { if (frames) *frames = 0; return HA_INVALID_ARGUMENT; }
    return read_pcm(s, &s->capture, &s->read_gate, dst, capacity, frames, out);
}

ha_result HA_CALL ha_stream_read_reference(ha_stream *s, void *dst, uint32_t capacity,
                                           uint32_t *frames, ha_block_info *out)
{
    if (!s) { if (frames) *frames = 0; return HA_INVALID_ARGUMENT; }
    return read_pcm(s, &s->reference, &s->reference_gate, dst, capacity, frames, out);
}

ha_result HA_CALL ha_stream_write(ha_stream *s, const void *src, uint32_t frames,
                                  uint64_t generation, uint32_t *accepted)
{
    uint32_t state, n;
    ha_block_info info = HA_INIT(ha_block_info);
    if (!accepted) return HA_INVALID_ARGUMENT;
    *accepted = 0;
    if (!s || frames > HA_MAX_QUANTUM || (frames && !src)) return HA_INVALID_ARGUMENT;
    if (!s->playback.slots) return HA_UNSUPPORTED;
    if (!hail_gate_enter(&s->write_gate)) return HA_BUSY;
    state = atomic_load_explicit(&s->state, memory_order_acquire);
    if (state != HA_STOPPED && state != HA_STARTING && state != HA_RUNNING) {
        hail_gate_leave(&s->write_gate); return HA_INVALID_STATE;
    }
    if (generation != s->generation) { hail_gate_leave(&s->write_gate); return HA_STALE_GENERATION; }
    info.generation = generation;
    n = hail_ring_push(&s->playback, src, frames, &info);
    hail_gate_leave(&s->write_gate);
    *accepted = n;
    return n || !frames ? HA_OK : HA_WOULD_BLOCK;
}

ha_result HA_CALL ha_stream_flush_playback(ha_stream *s, uint32_t flags, uint64_t *generation)
{
    uint32_t state;
    if (!s || !generation || (flags & ~HA_FLUSH_NATIVE_REQUIRED)) return HA_INVALID_ARGUMENT;
    if (!s->playback.slots) return HA_UNSUPPORTED;
    if ((flags & HA_FLUSH_NATIVE_REQUIRED) && !(s->info.features & HA_FEATURE_NATIVE_FLUSH))
        return HA_UNSUPPORTED;
    state = atomic_load_explicit(&s->state, memory_order_acquire);
    if (state != HA_STOPPED && state != HA_RUNNING) return HA_INVALID_STATE;
    if (s->flush_pending || !close_gate(&s->write_gate)) return HA_BUSY;
    if (s->generation == UINT64_MAX) { hail_gate_leave(&s->write_gate); return HA_OVERFLOW; }
    ++s->generation;
    s->flush_cutoff = atomic_load_explicit(&s->playback.write_index, memory_order_acquire);
    s->flush_pending = 1; s->flush_flags = flags; s->control_ticks = 0;
    atomic_store_explicit(&s->flush_requested, 1, memory_order_release);
    s->status.flush_result = HA_PENDING;
    *generation = s->generation;
    return HA_PENDING;
}
