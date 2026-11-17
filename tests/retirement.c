/* A failed open has no public handle, but can still own native references.
 * Exercise core ownership without devices or timing assumptions. */
#include "hail_internal.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
static ha_result open_result, retire_result;
static unsigned frees, cleanups;
typedef struct native_state { unsigned retired; } native_state;
static void HA_CALL release(void *user, void *ptr, size_t bytes, size_t alignment)
{ ++frees; hail_host_deallocate(user, ptr, bytes, alignment); }
static ha_result probe(ha_context *c, uint64_t id, const ha_format *f, uint32_t n, ha_probe_info *p)
{ (void)c; (void)id; (void)n; p->application_format = *f; return HA_OK; }
static ha_result open_stream(ha_stream *s) { (void)s; return open_result; }
static ha_result retire(ha_stream *s)
{
    native_state *n = s->backend_state;
    if (retire_result == HA_OK) n->retired = 1;
    return retire_result;
}
static void cleanup(ha_stream *s)
{ CHECK(((native_state *)s->backend_state)->retired); ++cleanups; }
static const hail_backend_ops backend = {
    .execution = HAIL_EXEC_CONCURRENT,
    .id = HA_BACKEND_ALSA, .probe = probe, .open = open_stream,
    .retire = retire, .stream_free = cleanup, .stream_bytes = sizeof(native_state)
};
static ha_context *context(void)
{
    ha_context_config cc = HA_INIT(ha_context_config); ha_context *c = NULL;
    cc.backend = HA_BACKEND_VIRTUAL; cc.max_streams = 1;
    cc.allocator.struct_size = sizeof(ha_allocator); cc.allocator.struct_version = HA_STRUCT_VERSION;
    cc.allocator.allocate = hail_host_allocate; cc.allocator.deallocate = release;
    CHECK(ha_context_create(&cc, &c) == HA_OK); c->backend = &backend; return c;
}
static void poll_context(ha_context *c)
{ uint32_t count; CHECK(ha_context_poll(c, NULL, 0, &count) == HA_OK); }
static void terminal_cause(void)
{
    ha_context c = {0}; ha_stream s = {0}; ha_stream_status status = HA_INIT(ha_stream_status);
    c.backend = &backend; s.context = &c; s.concurrent = 1;
    atomic_init(&s.state, HA_STARTING); hail_rt_init(&s);
    hail_rt_open(&s, 0); CHECK(!hail_rt_enter(&s, 0));
    atomic_store(&s.state, HA_RUNNING); CHECK(hail_rt_enter(&s, 0)); hail_rt_leave(&s, 0);
    hail_rt_error(&s, 0, HA_DEVICE_LOST, 11);
    hail_rt_error(&s, 1, HA_PERMISSION_DENIED, 22);
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(ha_stream_get_status(&s, &status) == HA_OK);
        CHECK(status.terminal_result == HA_DEVICE_LOST);
        CHECK(status.diagnostic.result == HA_DEVICE_LOST && status.diagnostic.native_code == 11);
    }
    atomic_store(&s.state, HA_CLOSING);
    CHECK(ha_stream_get_status(&s, &status) == HA_OK && status.diagnostic.native_code == 11);
}
int main(void)
{
    ha_stream_config sc = HA_INIT(ha_stream_config);
    ha_device_selector d = HA_INIT(ha_device_selector);
    ha_format f = HA_FORMAT_INIT(48000, 1, HA_PCM_S16);
    ha_memory_plan plan = HA_INIT(ha_memory_plan);
    ha_context *c; ha_stream *s = NULL;
    uint64_t baseline;
    sc.directions = HA_PLAYBACK; sc.playback_device = d; sc.playback_format = f;
    c = context(); baseline = c->allocated;
    CHECK(ha_stream_plan(c, &sc, &plan) == HA_OK);
    open_result = HA_PERMISSION_DENIED; retire_result = HA_STREAM_FAILED;
    CHECK(ha_stream_open(c, &sc, &s) == HA_PERMISSION_DENIED && !s);
    CHECK(c->stream_count == 1 && c->allocated == baseline + plan.bytes && !frees && !cleanups);
    CHECK(ha_stream_open(c, &sc, &s) == HA_RESOURCE_EXHAUSTED && !s);
    poll_context(c); CHECK(!frees && !cleanups);
    CHECK(ha_context_destroy(&c) == HA_PENDING && c && !frees);
    retire_result = HA_OK;
    CHECK(ha_context_destroy(&c) == HA_OK && !c && frees == 2 && cleanups == 1);

    c = context(); baseline = c->allocated;
    retire_result = HA_PENDING;
    CHECK(ha_stream_open(c, &sc, &s) == HA_PERMISSION_DENIED && !s);
    poll_context(c); CHECK(frees == 2 && cleanups == 1);
    retire_result = HA_OK; poll_context(c);
    CHECK(c->stream_count == 0 && c->allocated == baseline && frees == 3 && cleanups == 2);
    open_result = HA_OK;
    CHECK(ha_stream_open(c, &sc, &s) == HA_OK && s);
    retire_result = HA_STREAM_FAILED;
    CHECK(ha_stream_close_begin(s) == HA_PENDING); poll_context(c);
    CHECK(ha_stream_close_finish(&s) == HA_PENDING && s && frees == 3);
    CHECK(ha_context_destroy(&c) == HA_BUSY && c);
    retire_result = HA_OK; poll_context(c);
    CHECK(ha_stream_close_finish(&s) == HA_OK && !s && frees == 4 && cleanups == 3);
    CHECK(ha_stream_close_finish(&s) == HA_OK && frees == 4);
    CHECK(ha_context_destroy(&c) == HA_OK && !c && frees == 5);
    terminal_cause();
    puts("retirement retains ownership; terminal diagnostics preserve one coherent cause");
    return 0;
}
