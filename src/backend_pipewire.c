#define _POSIX_C_SOURCE 200809L
#include "hail_internal.h"
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/buffer/meta.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>

typedef struct pw_hail_context {
    ha_context *owner;
    struct pw_thread_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct pw_metadata *metadata;
    struct spa_hook core_hook, registry_hook, metadata_hook;
    hail_device nodes[HA_MAX_DEVICES];
    uint32_t count, overflow, metadata_id;
    int sequence, done, error;
    char defaults[2][256];
} pw_hail_context;
typedef struct pw_hail_leg {
    ha_stream *owner;
    struct pw_stream *stream;
    uint32_t leg, ready;
    enum pw_stream_state state;
    struct spa_hook hook;
    atomic_uint prepared, callbacks;
} pw_hail_leg;
typedef struct pw_hail_stream { pw_hail_leg legs[2]; } pw_hail_stream;

static pthread_once_t pw_once = PTHREAD_ONCE_INIT;
static void pipewire_global_init(void) { pw_init(NULL, NULL); }

static void core_done(void *data, uint32_t id, int sequence)
{
    pw_hail_context *p = data;
    if (id == PW_ID_CORE && sequence == p->sequence) { p->done = 1; pw_thread_loop_signal(p->loop, false); }
}
static void core_error(void *data, uint32_t id, int seq, int result, const char *message)
{
    pw_hail_context *p = data;
    (void)id; (void)seq; (void)message;
    p->error = result; pw_thread_loop_signal(p->loop, false);
}
static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS, .done = core_done, .error = core_error
};

static void default_name(char *out, size_t capacity, const char *json)
{
    const char *name = strstr(json, "\"name\""); size_t n = 0;
    out[0] = 0;
    if (!name || !(name = strchr(name + 6, ':'))) return;
    ++name; while (*name == ' ' || *name == '\t') ++name;
    if (*name++ != '"') return;
    while (*name && *name != '"') {
        if (*name == '\\' || n + 1u >= capacity) { out[0] = 0; return; }
        out[n++] = *name++;
    }
    if (*name != '"') { out[0] = 0; return; }
    out[n] = 0;
}
static int metadata_property(void *data, uint32_t subject, const char *key,
                               const char *type, const char *value)
{
    pw_hail_context *p = data;
    (void)subject; (void)type;
    if (!key) { p->defaults[0][0] = 0; p->defaults[1][0] = 0; }
    else if (!strcmp(key, "default.audio.source")) default_name(p->defaults[0], sizeof(p->defaults[0]), value ? value : "");
    else if (!strcmp(key, "default.audio.sink")) default_name(p->defaults[1], sizeof(p->defaults[1]), value ? value : "");
    atomic_store_explicit(&p->owner->registry_dirty, 1, memory_order_release);
    return 0;
}
static const struct pw_metadata_events metadata_events = {
    PW_VERSION_METADATA_EVENTS, .property = metadata_property
};

static void registry_global(void *data, uint32_t id, uint32_t permissions,
                              const char *type, uint32_t version, const struct spa_dict *props)
{
    pw_hail_context *p = data;
    const char *media, *name, *serial, *label;
    uint32_t direction;
    (void)permissions; (void)version;
    if (!props) return;
    if (!strcmp(type, PW_TYPE_INTERFACE_Metadata)) {
        name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (name && !strcmp(name, "default") && !p->metadata) {
            p->metadata = pw_registry_bind(p->registry, id, type, PW_VERSION_METADATA, 0);
            if (p->metadata) { p->metadata_id = id; pw_metadata_add_listener(p->metadata, &p->metadata_hook, &metadata_events, p); }
        }
        return;
    }
    if (strcmp(type, PW_TYPE_INTERFACE_Node)) return;
    media = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!media) return;
    if (!strcmp(media, "Audio/Source")) direction = HA_CAPTURE;
    else if (!strcmp(media, "Audio/Sink")) direction = HA_PLAYBACK;
    else return;
    name = spa_dict_lookup(props, PW_KEY_NODE_NAME); serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
    label = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    if (!name || !serial) return;
    if (p->count >= p->owner->max_devices || strlen(name) >= sizeof(p->nodes[0].native_name)) { p->overflow = 1; return; }
    if (hail_device_record(&p->nodes[p->count], direction, serial, label ? label : name, id) != HA_OK) {
        p->overflow = 1; return;
    }
    memcpy(p->nodes[p->count].native_name, name, strlen(name) + 1u);
    ++p->count;
    if (direction == HA_PLAYBACK) {
        char monitor[1024];
        if (p->count == p->owner->max_devices) { p->overflow = 1; return; }
        (void)snprintf(monitor, sizeof(monitor), "Monitor of %.900s", label ? label : name);
        if (hail_device_record(&p->nodes[p->count], HA_CAPTURE, serial, monitor, id) != HA_OK) { p->overflow = 1; return; }
        memcpy(p->nodes[p->count].native_name, name, strlen(name) + 1u);
        p->nodes[p->count].native_flags = 1;
        ++p->count;
    }
    atomic_store_explicit(&p->owner->registry_dirty, 1, memory_order_release);
}
static void registry_remove(void *data, uint32_t id)
{
    pw_hail_context *p = data; uint32_t i;
    if (p->metadata && p->metadata_id == id) {
        spa_hook_remove(&p->metadata_hook); pw_proxy_destroy((struct pw_proxy *)p->metadata);
        p->metadata = NULL; p->defaults[0][0] = 0; p->defaults[1][0] = 0;
    }
    for (i = 0; i < p->count;) {
        if (p->nodes[i].native_id == id) { --p->count; p->nodes[i] = p->nodes[p->count]; }
        else ++i;
    }
    atomic_store_explicit(&p->owner->registry_dirty, 1, memory_order_release);
}
static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS, .global = registry_global, .global_remove = registry_remove
};

static ha_result roundtrip(pw_hail_context *p)
{
    struct timespec deadline;
    p->done = 0; p->sequence = pw_core_sync(p->core, PW_ID_CORE, 0);
    if (p->sequence < 0) return HA_BACKEND_INIT_FAILED;
    if (pw_thread_loop_get_time(p->loop, &deadline, INT64_C(2000000000)) < 0) return HA_BACKEND_INIT_FAILED;
    while (!p->done && !p->error)
        if (pw_thread_loop_timed_wait_full(p->loop, &deadline) < 0) return HA_BACKEND_INIT_FAILED;
    if (p->error == -EACCES || p->error == -EPERM) return HA_PERMISSION_DENIED;
    return p->error ? HA_BACKEND_INIT_FAILED : HA_OK;
}
static ha_result pipewire_refresh(ha_context *c)
{
    pw_hail_context *p = c->backend_state;
    hail_device snapshot[HA_MAX_DEVICES];
    char defaults[2][1024] = {{0}};
    uint32_t i, count;
    ha_result r;
    pw_thread_loop_lock(p->loop);
    r = roundtrip(p);
    if (r == HA_OK) r = roundtrip(p); /* Include metadata bindings issued during initial globals. */
    if (r == HA_OK && p->overflow) r = HA_RESOURCE_EXHAUSTED;
    count = p->count;
    if (r == HA_OK) {
        memcpy(snapshot, p->nodes, count * sizeof(*snapshot));
        for (i = 0; i < count; ++i) {
            uint32_t leg = snapshot[i].info.direction == HA_CAPTURE ? 0u : 1u;
            if (!strcmp(snapshot[i].native_name, p->defaults[leg])) memcpy(defaults[leg], snapshot[i].key, strlen(snapshot[i].key) + 1u);
        }
    }
    pw_thread_loop_unlock(p->loop);
    return r == HA_OK ? hail_registry_publish(c, snapshot, count, defaults[0], defaults[1]) : r;
}
static ha_result pipewire_context_init(ha_context *c)
{
    pw_hail_context *p = c->backend_state;
    (void)pthread_once(&pw_once, pipewire_global_init);
    p->owner = c; p->loop = pw_thread_loop_new("hail-control", NULL);
    if (!p->loop) return HA_RESOURCE_EXHAUSTED;
    p->context = pw_context_new(pw_thread_loop_get_loop(p->loop), NULL, 0);
    if (!p->context) return HA_BACKEND_INIT_FAILED;
    p->core = pw_context_connect(p->context, NULL, 0);
    if (!p->core) return errno == EACCES ? HA_PERMISSION_DENIED : HA_BACKEND_INIT_FAILED;
    pw_core_add_listener(p->core, &p->core_hook, &core_events, p);
    p->registry = pw_core_get_registry(p->core, PW_VERSION_REGISTRY, 0);
    if (!p->registry) return HA_BACKEND_INIT_FAILED;
    pw_registry_add_listener(p->registry, &p->registry_hook, &registry_events, p);
    if (pw_thread_loop_start(p->loop) < 0) return HA_BACKEND_INIT_FAILED;
    return pipewire_refresh(c);
}
static ha_result pipewire_context_free(ha_context *c)
{
    pw_hail_context *p = c->backend_state;
    if (p->loop) pw_thread_loop_stop(p->loop);
    if (p->metadata) pw_proxy_destroy((struct pw_proxy *)p->metadata);
    if (p->registry) pw_proxy_destroy((struct pw_proxy *)p->registry);
    if (p->core) pw_core_disconnect(p->core);
    if (p->context) pw_context_destroy(p->context);
    if (p->loop) pw_thread_loop_destroy(p->loop);
    memset(p, 0, sizeof(*p));
    return HA_OK;
}
static ha_result pipewire_probe(ha_context *c, uint64_t id, const ha_format *f,
                                 uint32_t negotiation, ha_probe_info *out)
{
    if (!hail_device_find(c, id)) return HA_DEVICE_UNAVAILABLE;
    if (negotiation == HA_EXACT_END_TO_END) return HA_UNSUPPORTED;
    out->application_format = *f; out->backend_format = *f; out->backend_format_valid = 1;
    out->conversion = HA_CONVERSION_SERVER;
    out->capabilities = HA_FEATURE_RENDER_REFERENCE | HA_FEATURE_NATIVE_FLUSH;
    return HA_OK;
}

static void stream_state(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
    pw_hail_leg *l = data;
    pw_hail_context *p = l->owner->context->backend_state;
    (void)old; (void)error;
    l->state = state;
    if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) l->ready = 1;
    if (state == PW_STREAM_STATE_ERROR || (state == PW_STREAM_STATE_UNCONNECTED && l->ready))
        hail_rt_error(l->owner, l->leg, HA_DEVICE_LOST, state);
    pw_thread_loop_signal(p->loop, false);
}
static void stream_format(void *data, uint32_t id, const struct spa_pod *param)
{
    pw_hail_leg *l = data;
    const ha_format *f = l->leg ? &l->owner->info.playback_format : &l->owner->info.capture_format;
    struct spa_audio_info_raw raw = {0};
    if (id != SPA_PARAM_Format || !param) return;
    if (spa_format_audio_raw_parse(param, &raw) < 0 || raw.rate != f->sample_rate || raw.channels != f->channels ||
        raw.format != (f->sample_format == HA_PCM_S16 ? SPA_AUDIO_FORMAT_S16 : SPA_AUDIO_FORMAT_F32))
        hail_rt_error(l->owner, l->leg, HA_FORMAT_UNSUPPORTED, 0);
    else {
        _Alignas(8) uint8_t buffer[256];
        struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const struct spa_pod *metadata = spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
        (void)pw_stream_update_params(l->stream, &metadata, 1);
    }
}
static void stream_process(void *data)
{
    pw_hail_leg *l = data; ha_stream *s;
    struct pw_buffer *buffer;
    struct spa_data *d;
    uint32_t frames, offset = 0, bpf;
    int entered = 0;
    uint64_t first_host = 0; uint32_t time_flags = 0, byte_start = 0;
    atomic_fetch_add_explicit(&l->callbacks, 1, memory_order_acquire);
    if (!atomic_load_explicit(&l->prepared, memory_order_acquire)) goto finished;
    s = l->owner; bpf = l->leg ? s->playback.bytes_per_frame : s->capture.bytes_per_frame;
    buffer = pw_stream_dequeue_buffer(l->stream);
    if (!buffer) goto finished;
    if (buffer->buffer->n_datas != 1) { hail_rt_error(s, l->leg, HA_STREAM_FAILED, 0); goto done; }
    d = &buffer->buffer->datas[0];
    if (!d->data || !d->chunk || !bpf) { hail_rt_error(s, l->leg, HA_STREAM_FAILED, 0); goto done; }
    if (l->leg) {
        frames = d->maxsize / bpf;
        if (buffer->requested && buffer->requested < frames) frames = (uint32_t)buffer->requested;
        if (frames > HA_MAX_QUANTUM) { hail_rt_error(s, l->leg, HA_STREAM_FAILED, 0); d->chunk->size = 0; goto done; }
        d->chunk->offset = 0; d->chunk->stride = (int32_t)bpf; d->chunk->size = frames * bpf;
    } else {
        if (d->chunk->offset > d->maxsize || d->chunk->size > d->maxsize - d->chunk->offset ||
            d->chunk->size % bpf || d->chunk->stride < 0 ||
            (d->chunk->stride && d->chunk->stride != (int32_t)bpf)) {
            hail_rt_error(s, l->leg, HA_STREAM_FAILED, 0); goto done;
        }
        offset = d->chunk->offset; frames = d->chunk->size / bpf;
        if (frames > HA_MAX_QUANTUM) { hail_rt_error(s, l->leg, HA_STREAM_FAILED, 0); goto done; }
    }
    entered = hail_rt_enter(s, l->leg);
    if (!entered) { if (l->leg) memset(d->data, 0, frames * bpf); goto done; }
    if (!l->leg) {
        const struct spa_meta_header *header = spa_buffer_find_meta_data(buffer->buffer, SPA_META_Header, sizeof(*header));
        if ((header && (header->flags & SPA_META_HEADER_FLAG_CORRUPTED)) || (d->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) {
            ha_result r = hail_native_capture_loss(s, frames, 1);
            if (r != HA_OK) hail_rt_error(s, 0, r, 0);
            goto done;
        }
        if (header && (header->flags & SPA_META_HEADER_FLAG_DISCONT)) {
            ha_result r = hail_native_capture_loss(s, 0, 1);
            if (r != HA_OK) { hail_rt_error(s, 0, r, 0); goto done; }
        }
    }
    byte_start = offset;
    {
        struct pw_time time = {0};
        uint32_t rate = l->leg ? s->info.playback_format.sample_rate : s->info.capture_format.sample_rate;
        if (pw_stream_get_time_n(l->stream, &time, sizeof(time)) == 0 && time.now >= 0 && time.rate.denom &&
            time.rate.num && time.delay >= 0 && time.delay <= 1920000 && time.rate.num <= 1000) {
            uint64_t delay = (uint64_t)time.delay * UINT64_C(1000000000) * time.rate.num / time.rate.denom;
            uint64_t frames_ns = (uint64_t)frames * UINT64_C(1000000000) / rate;
            if (l->leg) {
                uint64_t queued = time.queued <= 1920000 && time.buffered <= 1920000 ? time.queued + time.buffered : 0;
                uint64_t extra = queued * UINT64_C(1000000000) / rate;
                if (delay <= UINT64_MAX - extra && (uint64_t)time.now <= UINT64_MAX - delay - extra) {
                    first_host = (uint64_t)time.now + delay + extra; time_flags = HA_TIME_HOST_VALID | HA_TIME_ESTIMATED;
                }
            } else if (delay <= UINT64_MAX - frames_ns && (uint64_t)time.now >= delay + frames_ns) {
                first_host = (uint64_t)time.now - delay - frames_ns; time_flags = HA_TIME_HOST_VALID | HA_TIME_ESTIMATED;
            }
        }
    }
    while (frames) {
        uint32_t n = frames > s->info.quantum_frames ? s->info.quantum_frames : frames;
        ha_result r;
        uint64_t delta = (uint64_t)((offset - byte_start) / bpf) * UINT64_C(1000000000) /
            (l->leg ? s->info.playback_format.sample_rate : s->info.capture_format.sample_rate);
        uint64_t host = delta <= UINT64_MAX - first_host ? first_host + delta : 0;
        uint32_t flags = host || !delta ? time_flags : 0;
        if (l->leg) r = hail_native_render(s, (unsigned char *)d->data + offset, n, host, 0, UINT64_MAX, flags);
        else r = hail_native_capture(s, (unsigned char *)d->data + offset, n, host, 0, UINT64_MAX, flags);
        if (r != HA_OK) { hail_rt_error(s, l->leg, r, 0); break; }
        offset += n * bpf; frames -= n;
    }
done:
    (void)pw_stream_queue_buffer(l->stream, buffer);
    if (entered) hail_rt_leave(s, l->leg);
finished:
    atomic_fetch_sub_explicit(&l->callbacks, 1, memory_order_release);
}
static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS, .state_changed = stream_state, .param_changed = stream_format, .process = stream_process
};

static void pipewire_stream_free(ha_stream *s)
{
    pw_hail_context *p = s->context->backend_state; pw_hail_stream *a = s->backend_state; uint32_t i;
    pw_thread_loop_lock(p->loop);
    for (i = 0; i < 2; ++i) if (a->legs[i].stream) {
        atomic_store_explicit(&a->legs[i].prepared, 0, memory_order_release);
        /* PipeWire's hook removal synchronously clears RT callbacks on the
         * data loop. It is the physical callback-retirement barrier. */
        spa_hook_remove(&a->legs[i].hook);
        (void)atomic_load_explicit(&a->legs[i].callbacks, memory_order_acquire);
        pw_stream_destroy(a->legs[i].stream); a->legs[i].stream = NULL;
    }
    pw_thread_loop_unlock(p->loop);
}
static ha_result pipewire_open(ha_stream *s)
{
    pw_hail_context *p = s->context->backend_state; pw_hail_stream *a = s->backend_state;
    uint32_t i; ha_result result = HA_OK;
    pw_thread_loop_lock(p->loop);
    for (i = 0; i < 2; ++i) {
        uint64_t id = i ? s->info.playback_device_id : s->info.capture_device_id;
        const ha_format *f = i ? &s->info.playback_format : &s->info.capture_format;
        const struct spa_pod *params[1]; _Alignas(8) uint8_t bytes[1024];
        struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(bytes, sizeof(bytes));
        struct spa_audio_info_raw raw = {0};
        struct pw_properties *props;
        hail_device *device;
        if (!id) continue;
        device = hail_device_find(s->context, id);
        if (!device) { result = HA_DEVICE_UNAVAILABLE; break; }
        props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, i ? "Playback" : "Capture",
            PW_KEY_MEDIA_ROLE, "Communication", PW_KEY_TARGET_OBJECT, device->key,
            "node.dont-reconnect", "true", "node.dont-move", "true", NULL);
        if (!props) { result = HA_RESOURCE_EXHAUSTED; break; }
        if (!i && device->native_flags) (void)pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
        a->legs[i].owner = s; a->legs[i].leg = i;
        atomic_init(&a->legs[i].prepared, 0); atomic_init(&a->legs[i].callbacks, 0);
        a->legs[i].stream = pw_stream_new(p->core, i ? "Hail playback" : "Hail capture", props);
        if (!a->legs[i].stream) { result = HA_RESOURCE_EXHAUSTED; break; }
        pw_stream_add_listener(a->legs[i].stream, &a->legs[i].hook, &stream_events, &a->legs[i]);
        atomic_store_explicit(&a->legs[i].prepared, 1, memory_order_release);
        raw.format = f->sample_format == HA_PCM_S16 ? SPA_AUDIO_FORMAT_S16 : SPA_AUDIO_FORMAT_F32;
        raw.rate = f->sample_rate; raw.channels = f->channels;
        raw.position[0] = f->channels == 1 ? SPA_AUDIO_CHANNEL_MONO : SPA_AUDIO_CHANNEL_FL;
        if (f->channels == 2) raw.position[1] = SPA_AUDIO_CHANNEL_FR;
        params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &raw);
        if (pw_stream_connect(a->legs[i].stream, i ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT, PW_ID_ANY,
            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_INACTIVE | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_DONT_RECONNECT | PW_STREAM_FLAG_RT_PROCESS,
            params, 1) < 0) { result = HA_STREAM_FAILED; break; }
    }
    if (result == HA_OK) result = roundtrip(p);
    if (result == HA_OK) {
        struct timespec deadline;
        (void)pw_thread_loop_get_time(p->loop, &deadline, INT64_C(2000000000));
        for (i = 0; i < 2; ++i) if (a->legs[i].stream) {
            while (!a->legs[i].ready && a->legs[i].state != PW_STREAM_STATE_ERROR)
                if (pw_thread_loop_timed_wait_full(p->loop, &deadline) < 0) { result = HA_DEVICE_UNAVAILABLE; break; }
            if (atomic_load_explicit(&s->rt[i].error, memory_order_acquire)) result = HA_STREAM_FAILED;
        }
    }
    pw_thread_loop_unlock(p->loop);
    return result;
}
static ha_result pipewire_active(ha_stream *s, int active)
{
    pw_hail_context *p = s->context->backend_state; pw_hail_stream *a = s->backend_state;
    uint32_t i; ha_result r = HA_OK;
    pw_thread_loop_lock(p->loop);
    for (i = 0; i < 2; ++i) if (a->legs[i].stream) {
        if (pw_stream_set_active(a->legs[i].stream, active != 0) < 0) r = HA_STREAM_FAILED;
        if (!active) (void)pw_stream_flush(a->legs[i].stream, false);
    }
    if (r != HA_OK && active) for (i = 0; i < 2; ++i)
        if (a->legs[i].stream) (void)pw_stream_set_active(a->legs[i].stream, false);
    pw_thread_loop_unlock(p->loop); return r;
}
static ha_result pipewire_start(ha_stream *s) { return pipewire_active(s, 1); }
static ha_result pipewire_stop(ha_stream *s) { return pipewire_active(s, 0); }
static ha_result pipewire_flush(ha_stream *s)
{
    pw_hail_context *p = s->context->backend_state; pw_hail_stream *a = s->backend_state;
    int e; ha_result r;
    pw_thread_loop_lock(p->loop);
    e = pw_stream_set_active(a->legs[1].stream, false);
    r = e < 0 ? HA_STREAM_FAILED : roundtrip(p);
    if (r == HA_OK && pw_stream_flush(a->legs[1].stream, false) < 0) r = HA_STREAM_FAILED;
    if (r == HA_OK && atomic_load_explicit(&s->state, memory_order_acquire) == HA_RUNNING) {
        if (pw_stream_set_active(a->legs[1].stream, true) < 0) r = HA_STREAM_FAILED;
        else r = roundtrip(p);
    }
    pw_thread_loop_unlock(p->loop); return r;
}
static ha_result pipewire_retire(ha_stream *s) { pipewire_stream_free(s); return HA_OK; }
const hail_backend_ops hail_pipewire_backend = {
    .execution = HAIL_EXEC_CONCURRENT,
    .id = HA_BACKEND_PIPEWIRE, .features = HA_FEATURE_PIPEWIRE | HA_FEATURE_RENDER_REFERENCE | HA_FEATURE_NATIVE_FLUSH,
    .context_init = pipewire_context_init, .probe = pipewire_probe, .open = pipewire_open,
    .start = pipewire_start, .stop = pipewire_stop, .flush = pipewire_flush, .retire = pipewire_retire,
    .context_bytes = sizeof(pw_hail_context), .stream_bytes = sizeof(pw_hail_stream),
    .refresh = pipewire_refresh, .context_free = pipewire_context_free, .stream_free = pipewire_stream_free
};
