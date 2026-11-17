#include "support.h"
/* Scripted adapter; never linked to an SDK. Gates model admitted operations
 * paused before retirement. Scheduling is byte-driven and single-threaded. */
typedef struct script { unsigned phase, held, delay, ticks, cleaned; } script;
static script plan;
static ha_result probe(ha_context *c, uint64_t id, const ha_format *f, uint32_t policy, ha_probe_info *out)
{ (void)c; (void)id; (void)policy; out->application_format = *f; return HA_OK; }
static ha_result open_adapter(ha_stream *s)
{ memset(s->backend_state, 0xa5, 16); return plan.phase == 1 ? HA_STREAM_FAILED : HA_OK; }
static ha_result start_adapter(ha_stream *s)
{ (void)s; if (plan.ticks++ < plan.delay) return HA_PENDING; return plan.phase == 2 ? HA_PERMISSION_DENIED : HA_OK; }
static ha_result stop_adapter(ha_stream *s)
{ (void)s; return plan.phase == 3 ? HA_STREAM_FAILED : HA_OK; }
static ha_result flush_adapter(ha_stream *s)
{
    if (plan.phase == 4) return HA_STREAM_FAILED;
    if (!s->concurrent) {
        s->playback.offset = 0;
        atomic_store_explicit(&s->playback.read_index, s->flush_cutoff, memory_order_release);
    }
    return HA_OK;
}
static ha_result retire_adapter(ha_stream *s)
{ (void)s; return plan.held ? HA_PENDING : HA_OK; }
static void free_adapter(ha_stream *s)
{
    CHECK(!plan.held);
    for (unsigned i = 0; i < 16; ++i) CHECK(((unsigned char *)s->backend_state)[i] == 0xa5);
    ++plan.cleaned;
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fuzz_input in = {data, size, 0}; fuzz_memory m = {0};
    ha_context_config cc = context_config(&m); ha_context *c = NULL; ha_stream *s = NULL;
    ha_stream_config sc = stream_config(1u + byte(&in) % 64u);
    unsigned mode = byte(&in), schedule = byte(&in);
    plan = (script){0}; plan.phase = byte(&in) % 6u; plan.delay = byte(&in) % 8u;
    hail_backend_ops b = { .id = HA_BACKEND_ALSA,
        .execution = (mode & 1u) ? HAIL_EXEC_CONCURRENT : HAIL_EXEC_SERIALIZED,
        .features = HA_FEATURE_RENDER_REFERENCE | HA_FEATURE_NATIVE_FLUSH,
        .probe = probe, .open = open_adapter, .start = start_adapter, .stop = stop_adapter,
        .flush = flush_adapter, .retire = retire_adapter, .stream_free = free_adapter, .stream_bytes = 16 };
    CHECK(ha_context_create(&cc, &c) == HA_OK); size_t baseline = m.live;
    c->backend = &b; plan.held = 1;
    ha_result r = ha_stream_open(c, &sc, &s);
    if (plan.phase == 1) {
        CHECK(r == HA_STREAM_FAILED && !s && c->stream_count == 1 && m.live > baseline);
        unsigned calls = m.calls;
        for (unsigned i = 0; i < (schedule & 7u) + 1u; ++i) {
            poll_context(c); CHECK(m.live > baseline && !plan.cleaned);
        }
        CHECK(ha_stream_open(c, &sc, &s) == HA_RESOURCE_EXHAUSTED && !s && m.calls == calls);
        CHECK(ha_context_destroy(&c) == HA_PENDING && c && !plan.cleaned);
        plan.held = 0; CHECK(ha_context_destroy(&c) == HA_OK && !c);
    } else {
        CHECK(r == HA_OK && s); unsigned calls = m.calls;
        CHECK(ha_stream_start(s) == HA_PENDING);
        for (unsigned i = 0; i <= plan.delay; ++i) poll_context(c);
        if (plan.phase == 2) CHECK(status(s).terminal_result == HA_PERMISSION_DENIED);
        else {
            CHECK(status(s).state == HA_RUNNING);
            /* Select an admitted callback, then pause control at flush/stop. */
            unsigned leg = schedule & 1u, active = 0;
            if (b.execution == HAIL_EXEC_CONCURRENT && (schedule & 2u)) {
                CHECK(hail_rt_enter(s, leg)); active = 1;
            }
            if (plan.phase == 3) {
                CHECK(ha_stream_stop(s) == HA_PENDING); poll_context(c);
                if (active) CHECK(status(s).state == HA_STOPPING);
            } else if (plan.phase == 4) {
                uint64_t gen; CHECK(ha_stream_flush_playback(s, 0, &gen) == HA_PENDING); poll_context(c);
                if (active && leg == 1) CHECK(status(s).flush_result == HA_PENDING);
            } else if (plan.phase == 5) {
                hail_rt_error(s, leg, HA_DEVICE_LOST, 123);
            }
            if (active) hail_rt_leave(s, leg);
            poll_context(c);
            if (plan.phase == 0) {
                int16_t old[64], fresh[64], output[64], capture[64]; uint32_t n;
                uint64_t generation = status(s).playback_generation;
                ha_block_info block = HA_INIT(ha_block_info);
                for (unsigned i = 0; i < 64; ++i) { old[i] = 111; fresh[i] = 222; }
                CHECK(ha_stream_write(s, old, sc.quantum_frames, generation, &n) == HA_OK && n == sc.quantum_frames);
                if (s->concurrent) CHECK(hail_rt_enter(s, 0));
                CHECK(hail_native_capture(s, fresh, sc.quantum_frames, 0, 0, UINT64_MAX, 0) == HA_OK);
                if (s->concurrent) hail_rt_leave(s, 0);
                uint64_t next;
                CHECK(ha_stream_flush_playback(s, 0, &next) == HA_PENDING && next == generation + 1);
                poll_context(c); CHECK(status(s).flush_result == HA_OK);
                CHECK(ha_stream_write(s, old, sc.quantum_frames, generation, &n) == HA_STALE_GENERATION && !n);
                if (s->concurrent) CHECK(hail_rt_enter(s, 1));
                CHECK(hail_native_render(s, output, sc.quantum_frames, 0, 0, UINT64_MAX, 0) == HA_OK);
                if (s->concurrent) hail_rt_leave(s, 1);
                for (unsigned i = 0; i < sc.quantum_frames; ++i) CHECK(output[i] == 0);
                CHECK(ha_stream_read(s, capture, sc.quantum_frames, &n, &block) == HA_OK && n == sc.quantum_frames);
                CHECK(!memcmp(capture, fresh, n * sizeof(int16_t)));
                CHECK(ha_stream_write(s, fresh, sc.quantum_frames, next, &n) == HA_OK && n == sc.quantum_frames);
                if (s->concurrent) CHECK(hail_rt_enter(s, 1));
                CHECK(hail_native_render(s, output, sc.quantum_frames, 0, 0, UINT64_MAX, 0) == HA_OK);
                if (s->concurrent) hail_rt_leave(s, 1);
                CHECK(!memcmp(output, fresh, sc.quantum_frames * sizeof(int16_t)));
            }
            if (plan.phase == 3 || plan.phase == 4) CHECK(status(s).terminal_result == HA_STREAM_FAILED);
            if (plan.phase == 5) CHECK(status(s).terminal_result == HA_DEVICE_LOST && status(s).diagnostic.native_code == 123);
        }
        ha_stream_status st = status(s);
        if (st.terminal_result != HA_OK) {
            hail_rt_error(s, 0, HA_OVERFLOW, 456); poll_context(c);
            CHECK(status(s).terminal_result == st.terminal_result && status(s).diagnostic.native_code == st.diagnostic.native_code);
        }
        /* Hold an application role across close and independently withhold the
         * adapter's retirement acknowledgement. Neither alone permits free. */
        atomic_uint *gate = (schedule % 3u == 0) ? &s->read_gate : (schedule % 3u == 1) ? &s->write_gate : &s->reference_gate;
        /* A failed flush can already have revoked the writer's admission. */
        if (atomic_load(gate) != HAIL_GATE_IDLE) gate = &s->read_gate;
        CHECK(hail_gate_enter(gate)); CHECK(ha_stream_close_begin(s) == HA_PENDING);
        size_t live = m.live;
        for (unsigned i = 0; i < (schedule & 7u) + 1u; ++i) {
            poll_context(c); CHECK(ha_stream_close_finish(&s) == HA_PENDING && s && m.live == live && !plan.cleaned);
        }
        if (schedule & 4u) {
            plan.held = 0; poll_context(c); CHECK(ha_stream_close_finish(&s) == HA_PENDING && s);
            hail_gate_leave(gate);
        } else {
            hail_gate_leave(gate); poll_context(c); CHECK(ha_stream_close_finish(&s) == HA_PENDING && s);
            plan.held = 0;
        }
        close_stream(c, &s); CHECK(m.live == baseline && m.calls == calls);
        CHECK(ha_context_destroy(&c) == HA_OK && !c);
    }
    CHECK(!m.live && plan.cleaned == 1); return 0;
}
