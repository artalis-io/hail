#include "support.h"
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fuzz_input in = {data, size, 0}; fuzz_memory m = {0};
    ha_context_config cc = context_config(&m); ha_context *c = NULL; ha_stream *s = NULL;
    ha_stream_config sc = stream_config(1u + byte(&in) % 64u);
    ha_virtual_config vc = HA_INIT(ha_virtual_config); int16_t pcm[HA_MAX_QUANTUM] = {0};
    CHECK(ha_context_create(&cc, &c) == HA_OK); size_t baseline = m.live;
    for (unsigned i = 0; i < 256 && in.at < in.size; ++i) {
        uint8_t op = byte(&in), arg = byte(&in);
        if (!s) {
            CHECK(ha_stream_open(c, &sc, &s) == HA_OK); vc.faults = 0; vc.host_time_ns = 0;
            vc.start_delay_polls = arg & 3u; vc.stop_delay_polls = (arg >> 2) & 3u;
            vc.flush_delay_polls = (arg >> 4) & 3u; CHECK(ha_virtual_configure(s, &vc) == HA_OK);
        }
        ha_stream_status before = status(s); unsigned calls = m.calls;
        uint32_t n = UINT32_MAX; uint64_t generation = before.playback_generation;
        ha_block_info block = HA_INIT(ha_block_info); ha_result r;
        switch (op % 12u) {
        case 0:
            r = ha_stream_start(s);
            CHECK(r == (before.state == HA_STOPPED ? (s->flush_pending ? HA_BUSY : HA_PENDING) : HA_INVALID_STATE)); break;
        case 1:
            r = ha_stream_stop(s);
            CHECK(r == (before.state == HA_STOPPED ? HA_OK :
                (before.state == HA_STARTING || before.state == HA_RUNNING || before.state == HA_STOPPING) ? HA_PENDING : HA_INVALID_STATE)); break;
        case 2: poll_context(c); break;
        case 3: case 4:
            r = op % 12u == 3 ? ha_stream_read(s, pcm, arg, &n, &block) : ha_stream_read_reference(s, pcm, arg, &n, &block);
            CHECK(n <= arg && (r == HA_OK || n == 0));
            if (n) CHECK(block.frames == n && block.clock_epoch && block.frame_time_denominator); break;
        case 5:
            if (arg & 128u) --generation;
            r = ha_stream_write(s, pcm, arg & 127u, generation, &n);
            CHECK(n <= (arg & 127u) && (r == HA_OK || n == 0));
            if (generation != before.playback_generation &&
                (before.state == HA_RUNNING || before.state == HA_STARTING || before.state == HA_STOPPED) && !s->flush_pending)
                CHECK(r == HA_STALE_GENERATION); break;
        case 6:
            r = ha_stream_flush_playback(s, arg & 1u, &generation);
            if (r == HA_PENDING) CHECK(generation == before.playback_generation + 1u);
            else CHECK(status(s).playback_generation == before.playback_generation); break;
        case 7: {
            ha_virtual_step_info step = HA_INIT(ha_virtual_step_info);
            step.capture_frames = arg % (sc.quantum_frames + 1u); step.playback_frames = (arg >> 2) % (sc.quantum_frames + 1u);
            step.flags = (arg >> 5) & 7u; step.advance_ns = (uint64_t)arg * 1000000u;
            r = ha_virtual_step(s, &step, pcm, HA_MAX_QUANTUM, &block);
            CHECK(r == (before.state == HA_RUNNING ? HA_OK : HA_INVALID_STATE));
            if (r == HA_OK) vc.host_time_ns += step.advance_ns;
            break;
        }
        case 8: CHECK(ha_stream_close_begin(s) == HA_PENDING); break;
        case 9:
            r = ha_stream_close_finish(&s); CHECK(r == HA_OK || r == HA_PENDING || r == HA_INVALID_STATE);
            if (!s) CHECK(r == HA_OK && m.live == baseline); break;
        case 10: vc.faults = arg & 31u; CHECK(ha_virtual_configure(s, &vc) == HA_OK); break;
        case 11: CHECK(ha_context_destroy(&c) == HA_BUSY && c); break;
        }
        CHECK(m.calls == calls);
        if (s) {
            ha_stream_status after = status(s);
            CHECK(after.state <= HA_CLOSING && after.playback_generation >= before.playback_generation);
            if (before.terminal_result != HA_OK) CHECK(after.terminal_result == before.terminal_result);
        }
    }
    if (s) { vc.faults = 0; CHECK(ha_virtual_configure(s, &vc) == HA_OK); close_stream(c, &s); }
    CHECK(m.live == baseline && ha_context_destroy(&c) == HA_OK && !m.live); return 0;
}
