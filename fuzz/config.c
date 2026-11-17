#include "support.h"
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fuzz_input in = {data, size, 0}; fuzz_memory m = {0};
    ha_context *c = NULL; ha_stream *s = NULL;
    ha_context_config cc = context_config(&m); ha_stream_config sc = stream_config(32);
    ha_memory_plan cp = HA_INIT(ha_memory_plan), sp = HA_INIT(ha_memory_plan);
    uint32_t mask = word(&in); ha_result planned, opened;
    m.fail_call = byte(&in) % 4u;
    if (mask & 65536u) {
        ha_format f = HA_FORMAT_INIT(44100, 2, HA_PCM_F32);
        sc.capture_format = f; sc.playback_format = f;
    }
    if (mask & 131072u) sc.quantum_frames = HA_MAX_QUANTUM;
    if (mask & 262144u) cc.struct_version = word(&in);
    if (mask & 1u) cc.max_devices = word(&in);
    if (mask & 2u) cc.max_streams = word(&in);
    if (mask & 4u) cc.event_capacity = word(&in);
    if (mask & 8u) cc.memory_limit = word(&in);
    planned = ha_context_plan(&cc, &cp);
    if (planned != HA_OK) {
        CHECK(ha_context_create(&cc, &c) == planned && !c && !m.live && !m.calls);
        return 0;
    }
    /* Bound allocation and iteration work independently of fuzz-selected plans. */
    if (planned == HA_OK && cp.bytes <= cc.memory_limit && cp.bytes <= 1048576u && cc.max_streams <= 1u) {
        opened = ha_context_create(&cc, &c);
        if (opened == HA_OK) {
            CHECK(c && m.live == cp.bytes);
            if (mask & 16u) sc.directions = word(&in);
            if (mask & 32u) sc.quantum_frames = word(&in);
            if (mask & 64u) sc.capture_slots = word(&in);
            if (mask & 128u) sc.playback_slots = word(&in);
            if (mask & 256u) sc.reference_slots = word(&in);
            if (mask & 512u) sc.capture_format.sample_rate = word(&in);
            if (mask & 1024u) sc.playback_format.channels = word(&in);
            if (mask & 2048u) sc.capture_format.sample_format = word(&in);
            if (mask & 4096u) sc.playback_format.storage = word(&in);
            if (mask & 8192u) sc.struct_size = word(&in);
            if (mask & 16384u) sc.capture_format.struct_version = word(&in);
            if (mask & 32768u) sc.negotiation = word(&in);
            planned = ha_stream_plan(c, &sc, &sp); opened = ha_stream_open(c, &sc, &s);
            if (opened == HA_OK) {
                CHECK(planned == HA_OK && s && m.live == cp.bytes + sp.bytes);
                close_stream(c, &s); CHECK(m.live == cp.bytes);
            } else { CHECK(!s && m.live == cp.bytes); if (planned != HA_OK) CHECK(opened == planned); }
            CHECK(ha_context_destroy(&c) == HA_OK && !c);
        } else CHECK(!c && !m.live);
    }
    CHECK(!m.live && !m.ptr[0] && !m.ptr[1]); return 0;
}
