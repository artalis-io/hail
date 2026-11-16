#define _POSIX_C_SOURCE 200809L
#include "hail_internal.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
typedef struct fixture { ha_stream *stream; atomic_uint done; uint32_t leg; } fixture;
static ha_result noop(ha_stream *s) { (void)s; return HA_OK; }
static unsigned context_free_calls;
static ha_result context_free(ha_context *c) { (void)c; return ++context_free_calls == 1 ? HA_PENDING : HA_OK; }
static ha_result probe(ha_context *c, uint64_t id, const ha_format *f, uint32_t n, ha_probe_info *p)
{ (void)c; (void)id; (void)n; p->application_format = *f; p->device_format = *f; p->device_format_valid = 1; return HA_OK; }
static const hail_backend_ops threaded_backend = {
    .id = HA_BACKEND_ALSA, .features = HA_FEATURE_NATIVE_FLUSH | HA_FEATURE_RENDER_REFERENCE,
    .probe = probe, .open = noop, .start = noop, .stop = noop, .flush = noop, .retire = noop,
    .context_free = context_free
};
static void *audio_thread(void *data)
{
    fixture *f = data; ha_stream *s = f->stream; int16_t samples[32] = {0};
    while (!atomic_load_explicit(&f->done, memory_order_acquire)) {
        if (hail_rt_enter(s, f->leg)) {
            ha_result r;
            if (f->leg) r = hail_native_render(s, samples, 32, 0, 0, UINT64_MAX, 0);
            else r = hail_native_capture(s, samples, 32, 0, 0, UINT64_MAX, 0);
            CHECK(r == HA_OK); hail_rt_leave(s, f->leg);
        }
        sched_yield();
    }
    return NULL;
}
static void poll_context(ha_context *c)
{ uint32_t n; CHECK(ha_context_poll(c, NULL, 0, &n) == HA_OK); }
int main(void)
{
    ha_context_config cc = HA_INIT(ha_context_config); ha_stream_config sc = HA_INIT(ha_stream_config);
    ha_device_selector selector = HA_INIT(ha_device_selector);
    ha_format format = HA_FORMAT_INIT(48000, 1, HA_PCM_S16);
    ha_stream_status st = HA_INIT(ha_stream_status); ha_block_info info = HA_INIT(ha_block_info);
    ha_context *c; ha_stream *s; pthread_t threads[2]; fixture f[2];
    uint32_t i, n; uint64_t generation = 1, capture_lost = 0, underrun = 0;
    int16_t samples[32] = {0};
    cc.backend = HA_BACKEND_VIRTUAL; CHECK(ha_context_create(&cc, &c) == HA_OK);
    c->backend = &threaded_backend;
    sc.directions = 3; sc.quantum_frames = 32; sc.capture_slots = 2; sc.playback_slots = 2; sc.reference_slots = 2;
    sc.capture_device = selector; sc.playback_device = selector; sc.capture_format = format; sc.playback_format = format;
    CHECK(ha_stream_open(c, &sc, &s) == HA_OK); CHECK(ha_stream_start(s) == HA_PENDING); poll_context(c);
    CHECK(hail_rt_enter(s, 0));
    CHECK(hail_native_capture(s, samples, 33, 0, 0, 0, 0) == HA_INVALID_ARGUMENT);
    CHECK(hail_native_capture(s, NULL, 32, 0, 0, 0, 0) == HA_INVALID_ARGUMENT);
    CHECK(hail_native_capture(s, samples, 32, 1000000, 1000, 1, HA_TIME_HOST_VALID | HA_TIME_DEVICE_VALID) == HA_OK);
    hail_rt_leave(s, 0);
    CHECK(ha_stream_read(s, samples, 32, &n, &info) == HA_OK && info.clock_epoch == 1);
    CHECK(hail_rt_enter(s, 0));
    CHECK(hail_native_capture(s, samples, 32, 500000, 0, 1, HA_TIME_HOST_VALID | HA_TIME_DEVICE_VALID) == HA_OK);
    hail_rt_leave(s, 0);
    CHECK(ha_stream_read(s, samples, 32, &n, &info) == HA_OK && info.clock_epoch == 2 && (info.flags & HA_BLOCK_DISCONTINUITY));
    for (i = 0; i < 2; ++i) { f[i].stream = s; f[i].leg = i; atomic_init(&f[i].done, 0); CHECK(!pthread_create(&threads[i], NULL, audio_thread, &f[i])); }
    for (i = 0; i < 20000; ++i) {
        ha_result r;
        (void)ha_stream_read(s, samples, 32, &n, &info);
        (void)ha_stream_read_reference(s, samples, 32, &n, &info);
        r = ha_stream_write(s, samples, 32, generation, &n); CHECK(r == HA_OK || r == HA_WOULD_BLOCK || r == HA_BUSY);
        if (i % 37u == 0) {
            uint64_t next;
            r = ha_stream_flush_playback(s, 0, &next); CHECK(r == HA_PENDING || r == HA_BUSY);
            if (r == HA_PENDING) generation = next;
        }
        poll_context(c); CHECK(ha_stream_get_status(s, &st) == HA_OK);
        CHECK(st.state == HA_RUNNING && st.capture_lost_frames >= capture_lost && st.playback_underrun_frames >= underrun);
        capture_lost = st.capture_lost_frames; underrun = st.playback_underrun_frames;
        sched_yield();
    }
    memcpy(c->devices[0].key, "removed", 8);
    hail_registry_remove(c, "removed", 0, HA_CAPTURE);
    CHECK(ha_stream_get_status(s, &st) == HA_OK && st.state == HA_FAILED && st.terminal_result == HA_DEVICE_LOST);
    CHECK(ha_stream_close_begin(s) == HA_PENDING);
    for (i = 0; i < 2; ++i) atomic_store_explicit(&f[i].done, 1, memory_order_release);
    for (i = 0; i < 2; ++i) CHECK(!pthread_join(threads[i], NULL));
    poll_context(c); CHECK(ha_stream_close_finish(&s) == HA_OK);
    CHECK(ha_context_destroy(&c) == HA_PENDING && c);
    CHECK(ha_context_refresh(c) == HA_INVALID_STATE);
    CHECK(ha_context_destroy(&c) == HA_OK && !c);
    puts("native callback gates and snapshot handoff passed"); return 0;
}
