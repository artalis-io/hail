#define _POSIX_C_SOURCE 200809L
#include "hail_internal.h"
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <poll.h>
#include <errno.h>
#include <stdlib.h>
#include <alloca.h>
#include <time.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <stdio.h>

typedef struct alsa_context { int notify; } alsa_context;
typedef struct alsa_stream {
    snd_pcm_t *pcm[2];
    pthread_t thread;
    atomic_uint run, exit;
    uint32_t created, pending, offset;
    unsigned char capture[HA_MAX_QUANTUM * 8u], playback[HA_MAX_QUANTUM * 8u];
    struct pollfd descriptors[64];
    nfds_t descriptor_count;
} alsa_stream;

static ha_result alsa_result(int e)
{
    if (e == -EACCES || e == -EPERM) return HA_PERMISSION_DENIED;
    if (e == -ENODEV || e == -ENXIO || e == -ENOENT) return HA_DEVICE_LOST;
    if (e == -ENOMEM) return HA_RESOURCE_EXHAUSTED;
    if (e == -EBUSY) return HA_DEVICE_UNAVAILABLE;
    if (e == -EINVAL) return HA_FORMAT_UNSUPPORTED;
    return HA_STREAM_FAILED;
}

static ha_result alsa_refresh(ha_context *c)
{
    void **hints = NULL;
    hail_device devices[HA_MAX_DEVICES];
    uint32_t count = 0, i;
    const char *def_c = NULL, *def_p = NULL;
    ha_result r = HA_OK;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) return HA_BACKEND_INIT_FAILED;
    for (i = 0; hints[i]; ++i) {
        char *name = snd_device_name_get_hint(hints[i], "NAME");
        char *description = snd_device_name_get_hint(hints[i], "DESC");
        char *io = snd_device_name_get_hint(hints[i], "IOID");
        uint32_t leg;
        /* Enumerate physical PCM endpoints plus their explicit plug adapters,
         * the logical default, and the null test endpoint. Rate/DSP factories
         * and every surround/remix alias are not independent devices. */
        if (name && strcmp(name, "default") && strcmp(name, "null") &&
            strncmp(name, "hw:", 3) && strncmp(name, "plughw:", 7)) {
            free(name); free(description); free(io); continue;
        }
        if (description) { char *nl = strchr(description, '\n'); if (nl) *nl = 0; }
        if (name) for (leg = 0; leg < 2; ++leg) {
            if (io && !strcmp(io, leg ? "Input" : "Output")) continue;
            if (count == c->max_devices) { r = HA_RESOURCE_EXHAUSTED; break; }
            r = hail_device_record(&devices[count], leg ? HA_PLAYBACK : HA_CAPTURE,
                                  name, description ? description : name, 0);
            if (r != HA_OK) break;
            hail_device_persistent(&devices[count]);
            if (!strncmp(name, "hw:", 3) || !strncmp(name, "plughw:", 7)) {
                const char *card = strstr(name, "CARD="), *dev = strstr(name, "DEV=");
                char alias[128]; unsigned pcm = 0;
                if (card && dev) {
                    size_t length = strcspn(card + 5, ",");
                    if (length < sizeof(alias)) {
                        int index;
                        memcpy(alias, card + 5, length); alias[length] = 0;
                        index = snd_card_get_index(alias);
                        if (index >= 0 && index < 65535 && sscanf(dev + 4, "%5u", &pcm) == 1 && pcm < 65535)
                            devices[count].native_id = ((uint64_t)((unsigned)index + 1u) << 32) | (pcm + 1u);
                    }
                }
            }
            if (!strcmp(name, "default")) { if (leg) def_p = devices[count].key; else def_c = devices[count].key; }
            ++count;
        }
        free(name); free(description); free(io);
        if (r != HA_OK) break;
    }
    snd_device_name_free_hint(hints);
    if (r != HA_OK) return r;
    return hail_registry_publish(c, devices, count, def_c, def_p);
}

static ha_result alsa_context_init(ha_context *c)
{
    alsa_context *a = c->backend_state;
    a->notify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (a->notify >= 0) (void)inotify_add_watch(a->notify, "/dev/snd", IN_CREATE | IN_DELETE | IN_ATTRIB);
    return alsa_refresh(c);
}
static ha_result alsa_context_free(ha_context *c)
{
    alsa_context *a = c->backend_state;
    if (a->notify >= 0) { close(a->notify); a->notify = -1; }
    return HA_OK;
}
static void alsa_context_poll(ha_context *c)
{
    alsa_context *a = c->backend_state;
    _Alignas(8) unsigned char events[2048];
    ssize_t bytes = a->notify >= 0 ? read(a->notify, events, sizeof(events)) : -1;
    size_t offset = 0;
    while (bytes > 0 && (size_t)bytes - offset >= sizeof(struct inotify_event)) {
        const struct inotify_event *e = (const struct inotify_event *)(const void *)(events + offset);
        if (e->len > (size_t)bytes - offset - sizeof(*e)) break;
        if ((e->mask & IN_DELETE) && e->len) {
            unsigned card, device; char direction;
            if (sscanf(e->name, "pcmC%5uD%5u%c", &card, &device, &direction) == 3 && card < 65535 && device < 65535 &&
                (direction == 'c' || direction == 'p'))
                hail_registry_remove(c, NULL, ((uint64_t)(card + 1u) << 32) | (device + 1u), direction == 'c' ? HA_CAPTURE : HA_PLAYBACK);
        }
        offset += sizeof(*e) + e->len;
    }
    if (bytes > 0)
        atomic_store_explicit(&c->registry_dirty, 1, memory_order_release);
}

static int configure_pcm(snd_pcm_t *pcm, const ha_format *f, uint32_t quantum,
                           uint32_t negotiation)
{
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
    snd_pcm_uframes_t period = quantum, buffer;
    unsigned rate = f->sample_rate;
    int e, dir = 0;
    snd_pcm_format_t format = f->sample_format == HA_PCM_S16 ? SND_PCM_FORMAT_S16 : SND_PCM_FORMAT_FLOAT;
    snd_pcm_hw_params_alloca(&hw); snd_pcm_sw_params_alloca(&sw);
    if ((e = snd_pcm_hw_params_any(pcm, hw)) < 0 ||
        (e = snd_pcm_hw_params_set_rate_resample(pcm, hw, negotiation == HA_EXACT_APPLICATION)) < 0 ||
        (e = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (e = snd_pcm_hw_params_set_format(pcm, hw, format)) < 0 ||
        (e = snd_pcm_hw_params_set_channels(pcm, hw, f->channels)) < 0 ||
        (e = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, &dir)) < 0) return e;
    if (rate != f->sample_rate) return -EINVAL;
    if ((e = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, &dir)) < 0) return e;
    if (period > HA_MAX_QUANTUM) return -EINVAL;
    buffer = period * 4u;
    if ((e = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) < 0 ||
        (e = snd_pcm_hw_params(pcm, hw)) < 0 || (e = snd_pcm_sw_params_current(pcm, sw)) < 0 ||
        (e = snd_pcm_sw_params_set_avail_min(pcm, sw, period)) < 0 ||
        (e = snd_pcm_sw_params_set_start_threshold(pcm, sw, period)) < 0 ||
        (e = snd_pcm_sw_params(pcm, sw)) < 0) return e;
    return snd_pcm_prepare(pcm);
}

static int open_pcm(snd_pcm_t **pcm, const hail_device *d, const ha_format *f,
                      uint32_t q, uint32_t negotiation)
{
    int flags = SND_PCM_NONBLOCK, e;
    if (negotiation == HA_EXACT_END_TO_END)
        flags |= SND_PCM_NO_AUTO_RESAMPLE | SND_PCM_NO_AUTO_CHANNELS | SND_PCM_NO_AUTO_FORMAT;
    e = snd_pcm_open(pcm, d->key, d->info.direction == HA_CAPTURE ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK, flags);
    if (e < 0) return e;
    if (negotiation == HA_EXACT_END_TO_END && snd_pcm_type(*pcm) != SND_PCM_TYPE_HW &&
        snd_pcm_type(*pcm) != SND_PCM_TYPE_NULL) e = -ENOTSUP;
    else e = configure_pcm(*pcm, f, q, negotiation);
    if (e < 0) { snd_pcm_close(*pcm); *pcm = NULL; }
    return e;
}

static ha_result alsa_probe(ha_context *c, uint64_t id, const ha_format *f,
                             uint32_t negotiation, ha_probe_info *out)
{
    hail_device *d = hail_device_find(c, id);
    snd_pcm_t *pcm = NULL;
    int e;
    if (!d) return HA_DEVICE_UNAVAILABLE;
    e = open_pcm(&pcm, d, f, 256, negotiation);
    if (e < 0) return e == -ENOTSUP ? HA_UNSUPPORTED : alsa_result(e);
    out->application_format = *f; out->backend_format = *f; out->backend_format_valid = 1;
    if (snd_pcm_type(pcm) == SND_PCM_TYPE_HW || snd_pcm_type(pcm) == SND_PCM_TYPE_NULL) {
        out->device_format = *f; out->device_format_valid = 1;
    } else out->conversion = HA_CONVERSION_UNKNOWN;
    out->capabilities = HA_FEATURE_RENDER_REFERENCE | HA_FEATURE_NATIVE_FLUSH;
    snd_pcm_close(pcm); return HA_OK;
}

/* Called only by the owning audio worker while its leg gate is held. */
static void alsa_io_error(ha_stream *s, uint32_t leg, int error)
{
    alsa_stream *a = s->backend_state;
    if (error == -EAGAIN) return;
    if (error == -EPIPE || error == -ESTRPIPE) {
        int e = snd_pcm_prepare(a->pcm[leg]);
        if (!e && !leg) e = snd_pcm_start(a->pcm[leg]);
        if (e < 0) { hail_rt_error(s, leg, alsa_result(e), e); return; }
        if (leg) {
            if (s->playback_epoch == UINT64_MAX) hail_rt_error(s, leg, HA_OVERFLOW, 0);
            else ++s->playback_epoch;
            s->playback_flags |= HA_BLOCK_DISCONTINUITY;
            a->pending = 0; a->offset = 0;
        } else {
            ha_result r = hail_native_capture_loss(s, 0, 1);
            if (r != HA_OK) hail_rt_error(s, leg, r, 0);
        }
        hail_signal(s, leg, HA_EVENT_XRUN, HA_OK);
        hail_signal(s, leg, HA_EVENT_CLOCK, HA_OK);
    } else hail_rt_error(s, leg, alsa_result(error), error);
}

static void alsa_process(ha_stream *s, uint32_t leg)
{
    alsa_stream *a = s->backend_state;
    snd_pcm_sframes_t avail, n;
    uint32_t count;
    struct timespec time;
    ha_result r = HA_OK;
    if (!a->pcm[leg] || !hail_rt_enter(s, leg)) return;
    avail = snd_pcm_avail_update(a->pcm[leg]);
    if (avail < 0) {
        alsa_io_error(s, leg, (int)avail);
        hail_rt_leave(s, leg); return;
    }
    count = (uint32_t)((snd_pcm_uframes_t)avail > s->info.quantum_frames ? s->info.quantum_frames : (snd_pcm_uframes_t)avail);
    (void)clock_gettime(CLOCK_MONOTONIC, &time);
    if (!leg && count) {
        n = snd_pcm_readi(a->pcm[0], a->capture, count);
        if (n > (snd_pcm_sframes_t)count) hail_rt_error(s, leg, HA_STREAM_FAILED, n);
        else if (n > 0) {
            snd_pcm_sframes_t delay = 0; uint64_t host = 0; uint32_t flags = 0;
            if (!snd_pcm_delay(a->pcm[0], &delay) && delay >= 0 && delay <= 1920000 && !clock_gettime(CLOCK_MONOTONIC, &time)) {
                uint64_t now = (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
                uint64_t lag = ((uint64_t)delay + (uint64_t)n) * UINT64_C(1000000000) / s->info.capture_format.sample_rate;
                if (now >= lag) { host = now - lag; flags |= HA_TIME_HOST_VALID | HA_TIME_ESTIMATED; }
            }
            r = hail_native_capture(s, a->capture, (uint32_t)n, host, 0, UINT64_MAX, flags);
        } else if (n < 0) alsa_io_error(s, leg, (int)n);
    } else if (leg && count) {
        if (!a->pending) {
            snd_pcm_sframes_t delay = 0; uint64_t host = 0; uint32_t flags = 0;
            if (!snd_pcm_delay(a->pcm[1], &delay) && delay >= 0 && delay <= 1920000 && !clock_gettime(CLOCK_MONOTONIC, &time)) {
                uint64_t now = (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
                uint64_t lag = (uint64_t)delay * UINT64_C(1000000000) / s->info.playback_format.sample_rate;
                if (now <= UINT64_MAX - lag) { host = now + lag; flags = HA_TIME_HOST_VALID | HA_TIME_ESTIMATED; }
            }
            r = hail_native_render(s, a->playback, count, host, 0, UINT64_MAX, flags);
            a->pending = count; a->offset = 0;
        }
        n = snd_pcm_writei(a->pcm[1], a->playback + (size_t)a->offset * s->playback.bytes_per_frame, a->pending);
        if (n > (snd_pcm_sframes_t)a->pending) hail_rt_error(s, leg, HA_STREAM_FAILED, n);
        else if (n > 0) { a->pending -= (uint32_t)n; a->offset += (uint32_t)n; }
        else if (n < 0) alsa_io_error(s, leg, (int)n);
    }
    if (r != HA_OK) hail_rt_error(s, leg, r, 0);
    hail_rt_leave(s, leg);
}

static void *alsa_worker(void *user)
{
    ha_stream *s = user; alsa_stream *a = s->backend_state;
    while (!atomic_load_explicit(&a->exit, memory_order_acquire)) {
        if (atomic_load_explicit(&a->run, memory_order_acquire)) {
            (void)poll(a->descriptors, a->descriptor_count, 5);
            alsa_process(s, 0); alsa_process(s, 1);
        } else (void)poll(NULL, 0, 5);
    }
    return NULL;
}

static ha_result alsa_retire(ha_stream *s)
{
    alsa_stream *a = s->backend_state; uint32_t i;
    atomic_store_explicit(&a->run, 0, memory_order_release);
    atomic_store_explicit(&a->exit, 1, memory_order_release);
    if (a->created) {
        int e = pthread_join(a->thread, NULL);
        if (e) return alsa_result(-e);
        a->created = 0;
    }
    for (i = 0; i < 2; ++i) if (a->pcm[i]) { snd_pcm_close(a->pcm[i]); a->pcm[i] = NULL; }
    return HA_OK;
}
static ha_result alsa_open(ha_stream *s)
{
    alsa_stream *a = s->backend_state; uint32_t i; int e = 0;
    atomic_init(&a->run, 0); atomic_init(&a->exit, 0);
    for (i = 0; i < 2; ++i) {
        uint64_t id = i ? s->info.playback_device_id : s->info.capture_device_id;
        int descriptors;
        if (!id) continue;
        e = open_pcm(&a->pcm[i], hail_device_find(s->context, id),
                     i ? &s->info.playback_format : &s->info.capture_format,
                     s->info.quantum_frames, s->negotiation);
        if (e < 0) break;
        descriptors = snd_pcm_poll_descriptors_count(a->pcm[i]);
        if (descriptors < 0 || (unsigned)descriptors > 64u - a->descriptor_count) { e = -ENOMEM; break; }
        e = snd_pcm_poll_descriptors(a->pcm[i], a->descriptors + a->descriptor_count, (unsigned)descriptors);
        if (e < 0) break;
        a->descriptor_count += (nfds_t)descriptors;
    }
    if (e >= 0) { e = pthread_create(&a->thread, NULL, alsa_worker, s); if (!e) a->created = 1; else e = -e; }
    if (e < 0) return e == -ENOTSUP ? HA_UNSUPPORTED : alsa_result(e);
    s->info.clock_relationship = HA_CLOCK_UNKNOWN; return HA_OK;
}
static ha_result alsa_start(ha_stream *s)
{
    alsa_stream *a = s->backend_state; uint32_t i; int e;
    for (i = 0; i < 2; ++i) if (a->pcm[i]) {
        e = snd_pcm_prepare(a->pcm[i]);
        if (!e && !i) e = snd_pcm_start(a->pcm[i]);
        if (e < 0) { uint32_t j; for (j = 0; j < 2; ++j) if (a->pcm[j]) (void)snd_pcm_drop(a->pcm[j]); return alsa_result(e); }
    }
    atomic_store_explicit(&a->run, 1, memory_order_release); return HA_OK;
}
static ha_result alsa_stop(ha_stream *s)
{
    alsa_stream *a = s->backend_state; uint32_t i; ha_result r = HA_OK;
    atomic_store_explicit(&a->run, 0, memory_order_release);
    for (i = 0; i < 2; ++i) if (a->pcm[i]) {
        int e = snd_pcm_drop(a->pcm[i]);
        if (e < 0 && r == HA_OK) r = alsa_result(e);
    }
    a->pending = 0; a->offset = 0; return r;
}
static ha_result alsa_flush(ha_stream *s)
{
    alsa_stream *a = s->backend_state; int e;
    e = snd_pcm_drop(a->pcm[1]); if (e < 0) return alsa_result(e);
    e = snd_pcm_prepare(a->pcm[1]); a->pending = 0; a->offset = 0;
    return e < 0 ? alsa_result(e) : HA_OK;
}
const hail_backend_ops hail_alsa_backend = {
    .execution = HAIL_EXEC_CONCURRENT,
    .id = HA_BACKEND_ALSA, .features = HA_FEATURE_ALSA | HA_FEATURE_RENDER_REFERENCE | HA_FEATURE_NATIVE_FLUSH,
    .context_init = alsa_context_init, .probe = alsa_probe, .open = alsa_open,
    .start = alsa_start, .stop = alsa_stop, .flush = alsa_flush, .retire = alsa_retire,
    .context_bytes = sizeof(alsa_context), .stream_bytes = sizeof(alsa_stream),
    .refresh = alsa_refresh, .context_free = alsa_context_free,
    .context_poll = alsa_context_poll
};
