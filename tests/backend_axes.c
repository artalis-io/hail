/* The same backend identity must support either ownership model. Private
 * storage is requested by its adapter, not inferred from capture direction. */
#include "hail_internal.h"
#include <hail/virtual.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
static size_t extra_bytes;
static ha_result extra_result;
static unsigned allocations, releases;
static void *HA_CALL allocate(void *user, size_t bytes, size_t alignment)
{ ++allocations; return hail_host_allocate(user, bytes, alignment); }
static void HA_CALL release(void *user, void *ptr, size_t bytes, size_t alignment)
{ ++releases; hail_host_deallocate(user, ptr, bytes, alignment); }
static ha_context *context(void)
{
    ha_context_config cc = HA_INIT(ha_context_config); ha_context *c;
    cc.backend = HA_BACKEND_VIRTUAL;
    cc.allocator.struct_size = sizeof(ha_allocator); cc.allocator.struct_version = HA_STRUCT_VERSION;
    cc.allocator.allocate = allocate; cc.allocator.deallocate = release;
    CHECK(ha_context_create(&cc, &c) == HA_OK); return c;
}
static ha_stream_config config(uint32_t directions, uint32_t quantum)
{
    ha_stream_config sc = HA_INIT(ha_stream_config);
    ha_device_selector d = HA_INIT(ha_device_selector);
    ha_format f = HA_FORMAT_INIT(48000, 1, HA_PCM_S16);
    sc.directions = directions; sc.quantum_frames = quantum;
    sc.capture_device = d; sc.playback_device = d; sc.capture_format = f; sc.playback_format = f;
    if (directions & HA_CAPTURE) sc.capture_slots = 2;
    if (directions & HA_PLAYBACK) sc.playback_slots = 2;
    return sc;
}
static void poll_context(ha_context *c)
{ uint32_t count; CHECK(ha_context_poll(c, NULL, 0, &count) == HA_OK); }
static ha_stream_status status(ha_stream *s)
{ ha_stream_status st = HA_INIT(ha_stream_status); CHECK(ha_stream_get_status(s, &st) == HA_OK); return st; }
static void close_stream(ha_context *c, ha_stream **s)
{ CHECK(ha_stream_close_begin(*s) == HA_PENDING); poll_context(c); CHECK(ha_stream_close_finish(s) == HA_OK); }
static ha_result noop(ha_stream *s) { (void)s; return HA_OK; }
static ha_result probe(ha_context *c, uint64_t id, const ha_format *f, uint32_t policy, ha_probe_info *out)
{ (void)c; (void)id; (void)policy; out->application_format = *f; return HA_OK; }
static ha_result private_extra(const ha_stream_info *info, size_t *bytes)
{ CHECK(info->quantum_frames); *bytes = extra_bytes; return extra_result; }
static ha_result open_stream(ha_stream *s)
{ memset(s->backend_state, 0xa5, 16); return HA_OK; }
static void cleanup(ha_stream *s)
{ for (unsigned i = 0; i < 16; ++i) CHECK(((unsigned char *)s->backend_state)[i] == 0xa5); }
static const hail_backend_ops backend = {
    .id = HA_BACKEND_ALSA, .execution = HAIL_EXEC_SERIALIZED,
    .probe = probe, .open = open_stream, .start = noop, .stop = noop, .flush = noop, .retire = noop,
    .stream_bytes = 16, .stream_extra_bytes = private_extra, .stream_free = cleanup
};

static void ownership(uint32_t id, uint32_t execution)
{
    ha_context *c = context(); hail_backend_ops b = backend; ha_stream *s;
    ha_stream_config sc = config(HA_CAPTURE | HA_PLAYBACK, 4);
    ha_memory_plan plan = HA_INIT(ha_memory_plan); ha_stream_status st;
    ha_block_info block = HA_INIT(ha_block_info);
    int16_t input[] = {11, 22, 33, 44}, primed[] = {7, 8, 9, 10}, output[4], readback[4];
    uint32_t n; uint64_t baseline = c->allocated;
    b.id = id; b.execution = execution; c->backend = &b; sc.reference_slots = 2;
    CHECK(ha_stream_plan(c, &sc, &plan) == HA_OK);
    CHECK(ha_stream_open(c, &sc, &s) == HA_OK && c->allocated == baseline + plan.bytes);
    CHECK(ha_stream_write(s, primed, 4, 1, &n) == HA_OK && n == 4);
    CHECK(ha_stream_start(s) == HA_PENDING); poll_context(c); CHECK(status(s).state == HA_RUNNING);
    for (unsigned i = 0; i < 3; ++i) {
        if (execution == HAIL_EXEC_CONCURRENT) CHECK(hail_rt_enter(s, 0));
        CHECK(hail_native_capture(s, input, 4, 0, 0, UINT64_MAX, 0) == HA_OK);
        if (execution == HAIL_EXEC_CONCURRENT) hail_rt_leave(s, 0);
        if (execution == HAIL_EXEC_CONCURRENT) CHECK(hail_rt_enter(s, 1));
        CHECK(hail_native_render(s, output, 4, 0, 0, UINT64_MAX, 0) == HA_OK);
        if (execution == HAIL_EXEC_CONCURRENT) hail_rt_leave(s, 1);
        if (!i) CHECK(!memcmp(output, primed, sizeof(output)));
        else for (unsigned j = 0; j < 4; ++j) CHECK(output[j] == 0);
        st = status(s); /* Drain snapshots without assuming they cannot fill. */
    }
    CHECK(st.capture_lost_frames == 4 && st.playback_underrun_frames == 8 && st.reference_lost_frames == 4);
    CHECK(ha_stream_read(s, readback, 2, &n, &block) == HA_OK && n == 2 && block.first_frame == 0);
    CHECK(ha_stream_read(s, readback + 2, 2, &n, &block) == HA_OK && n == 2 && block.first_frame == 2);
    CHECK(!memcmp(readback, input, sizeof(input)));
    CHECK(ha_stream_read_reference(s, readback, 4, &n, &block) == HA_OK && n == 4);
    CHECK(!memcmp(readback, primed, sizeof(primed)));
    if (execution == HAIL_EXEC_CONCURRENT) CHECK(hail_rt_enter(s, 0));
    CHECK(hail_native_capture_loss(s, 3, 1) == HA_OK);
    CHECK(hail_native_capture(s, input, 4, 0, 0, UINT64_MAX, 0) == HA_OK);
    if (execution == HAIL_EXEC_CONCURRENT) hail_rt_leave(s, 0);
    CHECK(status(s).capture_lost_frames == 7);
    CHECK(ha_stream_read(s, readback, 4, &n, &block) == HA_OK && block.first_frame == 4);
    CHECK(ha_stream_read(s, readback, 4, &n, &block) == HA_OK && block.first_frame == 15);
    CHECK(block.lost_frames == 7 && block.clock_epoch == 2 && (block.flags & HA_BLOCK_UNKNOWN_GAP));
    if (execution == HAIL_EXEC_CONCURRENT) CHECK(hail_rt_enter(s, 1));
    CHECK(hail_native_render(s, output, 4, 0, 0, UINT64_MAX, 0) == HA_OK);
    if (execution == HAIL_EXEC_CONCURRENT) hail_rt_leave(s, 1);
    CHECK(ha_stream_read_reference(s, readback, 4, &n, &block) == HA_OK);
    CHECK(ha_stream_read_reference(s, readback, 4, &n, &block) == HA_OK && block.lost_frames == 4);
    CHECK(ha_stream_stop(s) == HA_PENDING); poll_context(c);
    st = status(s); CHECK(st.state == HA_STOPPED && st.playback_generation == 2);
    hail_rt_error(s, 0, HA_DEVICE_LOST, 17);
    st = status(s);
    CHECK(st.state == HA_FAILED && st.terminal_result == HA_DEVICE_LOST && st.diagnostic.native_code == 17);
    close_stream(c, &s); CHECK(c->allocated == baseline);
    CHECK(ha_context_destroy(&c) == HA_OK);
}

static void planning(void)
{
    ha_context *c = context(); hail_backend_ops b = backend; ha_stream *s = NULL;
    ha_stream_config capture = config(HA_CAPTURE, HA_MAX_QUANTUM), playback = config(HA_PLAYBACK, HA_MAX_QUANTUM);
    ha_memory_plan cp = HA_INIT(ha_memory_plan), pp = HA_INIT(ha_memory_plan), ep = HA_INIT(ha_memory_plan);
    ha_format format = HA_FORMAT_INIT(48000, 2, HA_PCM_F32);
    unsigned calls; uint64_t baseline = c->allocated;
    capture.capture_format = format; playback.playback_format = format;
    c->backend = &b;
    CHECK(ha_stream_plan(c, &capture, &cp) == HA_OK && ha_stream_plan(c, &playback, &pp) == HA_OK);
    CHECK(cp.bytes == pp.bytes); /* Equal rings + fixed private state: no generic capture scratch. */
    extra_bytes = 32; CHECK(ha_stream_plan(c, &capture, &ep) == HA_OK && ep.bytes == cp.bytes + 32);
    CHECK(ha_stream_open(c, &capture, &s) == HA_OK && c->allocated == baseline + ep.bytes);
    close_stream(c, &s); CHECK(c->allocated == baseline);
    calls = allocations;
    extra_result = HA_RESOURCE_EXHAUSTED;
    CHECK(ha_stream_plan(c, &capture, &cp) == HA_RESOURCE_EXHAUSTED);
    CHECK(ha_stream_open(c, &capture, &s) == HA_RESOURCE_EXHAUSTED && !s);
    extra_result = HA_OK; extra_bytes = SIZE_MAX;
    CHECK(ha_stream_plan(c, &capture, &cp) == HA_OVERFLOW);
    extra_bytes = 0; b.stream_bytes = SIZE_MAX;
    CHECK(ha_stream_plan(c, &capture, &cp) == HA_OVERFLOW);
    b.stream_bytes = 16; b.execution = 0;
    CHECK(ha_stream_plan(c, &capture, &cp) == HA_UNSUPPORTED);
    CHECK(allocations == calls && c->allocated == baseline);
    CHECK(ha_context_destroy(&c) == HA_OK);

    /* Synthetic scratch belongs to virtual capture only, at its maximum size. */
    c = context(); calls = allocations;
    CHECK(ha_stream_plan(c, &capture, &cp) == HA_OK && ha_stream_plan(c, &playback, &pp) == HA_OK);
    CHECK(cp.bytes == pp.bytes + (uint64_t)HA_MAX_QUANTUM * 8u && allocations == calls);
    CHECK(ha_stream_open(c, &capture, &s) == HA_OK && allocations == calls + 1);
    {
        ha_virtual_step_info step = HA_INIT(ha_virtual_step_info); ha_block_info block = HA_INIT(ha_block_info);
        static float pcm[HA_MAX_QUANTUM * 2u]; uint32_t n;
        CHECK(ha_stream_start(s) == HA_PENDING); poll_context(c);
        step.capture_frames = HA_MAX_QUANTUM;
        CHECK(ha_virtual_step(s, &step, NULL, 0, &block) == HA_OK);
        CHECK(ha_stream_read(s, pcm, HA_MAX_QUANTUM, &n, &block) == HA_OK && n == HA_MAX_QUANTUM);
        CHECK(pcm[0] == -1.0f && pcm[1] == -32751.0f / 32768.0f);
        CHECK(pcm[HA_MAX_QUANTUM * 2u - 2u] == (float)((int32_t)HA_MAX_QUANTUM - 1 - 32768) / 32768.0f);
        CHECK(allocations == calls + 1);
    }
    close_stream(c, &s); CHECK(ha_context_destroy(&c) == HA_OK);
}

static void virtual_progress(void)
{
    ha_context *c = context(); ha_stream *s; ha_stream_config sc = config(3, 4);
    ha_virtual_config vc = HA_INIT(ha_virtual_config); uint64_t generation;
    vc.start_delay_polls = 2; vc.stop_delay_polls = 2; vc.flush_delay_polls = 2;
    CHECK(ha_stream_open(c, &sc, &s) == HA_OK && ha_virtual_configure(s, &vc) == HA_OK);
    for (unsigned cycle = 0; cycle < 2; ++cycle) {
        CHECK(ha_stream_start(s) == HA_PENDING);
        poll_context(c); poll_context(c); CHECK(status(s).state == HA_STARTING);
        poll_context(c); CHECK(status(s).state == HA_RUNNING);
        for (unsigned flush = 0; flush < 2; ++flush) {
            CHECK(ha_stream_flush_playback(s, 0, &generation) == HA_PENDING);
            poll_context(c); poll_context(c); CHECK(status(s).flush_result == HA_PENDING);
            poll_context(c); CHECK(status(s).flush_result == HA_OK);
        }
        CHECK(ha_stream_flush_playback(s, 0, &generation) == HA_PENDING); poll_context(c);
        CHECK(ha_stream_stop(s) == HA_PENDING);
        CHECK(hail_gate_enter(&s->read_gate));
        poll_context(c); poll_context(c); poll_context(c); CHECK(status(s).state == HA_STOPPING);
        hail_gate_leave(&s->read_gate); poll_context(c); CHECK(status(s).state == HA_STOPPED);
    }
    CHECK(ha_stream_start(s) == HA_PENDING); poll_context(c);
    CHECK(ha_stream_stop(s) == HA_PENDING);
    poll_context(c); poll_context(c); CHECK(status(s).state == HA_STOPPING);
    poll_context(c); CHECK(status(s).state == HA_STOPPED);
    CHECK(ha_stream_start(s) == HA_PENDING);
    poll_context(c); poll_context(c); CHECK(status(s).state == HA_STARTING);
    poll_context(c); CHECK(status(s).state == HA_RUNNING);
    close_stream(c, &s); CHECK(ha_context_destroy(&c) == HA_OK);
}

int main(void)
{
    ownership(HA_BACKEND_ALSA, HAIL_EXEC_SERIALIZED);
    ownership(HA_BACKEND_ALSA, HAIL_EXEC_CONCURRENT);
    ownership(HA_BACKEND_VIRTUAL, HAIL_EXEC_SERIALIZED);
    ownership(HA_BACKEND_VIRTUAL, HAIL_EXEC_CONCURRENT);
    planning(); virtual_progress(); CHECK(allocations == releases);
    puts("backend identity, execution ownership and private storage vary independently");
    return 0;
}
