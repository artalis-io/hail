#include "hail_internal.h"
#include <hail/virtual.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
#define OK(x) CHECK((x) == HA_OK)
typedef struct tracker { unsigned calls, frees, live, fail; size_t bytes, peak; } tracker;
static void *HA_CALL allocate(void *user, size_t bytes, size_t alignment)
{
    tracker *t = user;
    uintptr_t p;
    void *base;
    ++t->calls;
    if (t->calls == t->fail) return NULL;
    base = malloc(bytes + alignment + sizeof(void *)); CHECK(base);
    p = ((uintptr_t)base + sizeof(void *) + alignment - 1u) & ~(uintptr_t)(alignment - 1u);
    ((void **)p)[-1] = base;
    ++t->live; t->bytes += bytes; if (t->bytes > t->peak) t->peak = t->bytes;
    return (void *)p;
}
static void HA_CALL deallocate(void *user, void *ptr, size_t bytes, size_t alignment)
{
    tracker *t = user;
    CHECK((uintptr_t)ptr % alignment == 0 && t->live && t->bytes >= bytes);
    --t->live; ++t->frees; t->bytes -= bytes;
    free(((void **)ptr)[-1]);
}
static ha_context_config context_cfg(tracker *t)
{
    ha_context_config v = HA_INIT(ha_context_config);
    v.backend = HA_BACKEND_VIRTUAL;
    v.allocator.struct_size = sizeof(ha_allocator); v.allocator.struct_version = 1;
    v.allocator.allocate = allocate; v.allocator.deallocate = deallocate; v.allocator.user = t;
    return v;
}
static ha_stream_config stream_cfg(uint32_t dirs)
{
    ha_stream_config v = HA_INIT(ha_stream_config);
    ha_format f = HA_FORMAT_INIT(48000, 1, HA_PCM_S16);
    ha_device_selector d = HA_INIT(ha_device_selector);
    v.directions = dirs; v.capture_format = f; v.playback_format = f;
    v.capture_device = d; v.playback_device = d;
    v.quantum_frames = 4;
    if (dirs & HA_CAPTURE) v.capture_slots = 2;
    if (dirs & HA_PLAYBACK) { v.playback_slots = 2; v.reference_slots = 2; }
    return v;
}
static void poll_context(ha_context *c)
{
    uint32_t count;
    OK(ha_context_poll(c, NULL, 0, &count)); CHECK(count == 0);
}
static ha_stream_status status(ha_stream *s)
{
    ha_stream_status v = HA_INIT(ha_stream_status);
    OK(ha_stream_get_status(s, &v)); return v;
}
static void close_stream(ha_context *c, ha_stream **s)
{
    CHECK(ha_stream_close_begin(*s) == HA_PENDING);
    poll_context(c); OK(ha_stream_close_finish(s)); CHECK(!*s);
    OK(ha_stream_close_finish(s));
}
static void start(ha_context *c, ha_stream *s)
{
    CHECK(ha_stream_start(s) == HA_PENDING); poll_context(c);
    CHECK(status(s).state == HA_RUNNING);
}
static void step(ha_stream *s, uint32_t capture, uint32_t playback, void *out, ha_block_info *info)
{
    ha_virtual_step_info v = HA_INIT(ha_virtual_step_info);
    v.capture_frames = capture; v.playback_frames = playback;
    OK(ha_virtual_step(s, &v, out, playback, info));
}

static void discovery_and_limits(void)
{
    tracker t = {0}; ha_context *c = NULL; ha_stream *s = NULL, *other = NULL;
    ha_context_config cfg = context_cfg(&t);
    ha_stream_config sc = stream_cfg(HA_CAPTURE | HA_PLAYBACK);
    ha_device_info devices[2] = { HA_INIT(ha_device_info), HA_INIT(ha_device_info) };
    ha_device_selector d = HA_INIT(ha_device_selector);
    ha_probe_info probe = HA_INIT(ha_probe_info);
    ha_memory_plan cp = HA_INIT(ha_memory_plan), sp = HA_INIT(ha_memory_plan);
    ha_context_status cs = HA_INIT(ha_context_status);
    uint32_t count; uint64_t id, old;
    cfg.max_streams = 1;
    OK(ha_context_plan(&cfg, &cp)); OK(ha_context_create(&cfg, &c));
    CHECK(t.bytes == cp.bytes && t.calls == 1);
    CHECK(ha_context_enumerate(c, NULL, 0, &count) == HA_TOO_SMALL && count == 2);
    OK(ha_context_enumerate(c, devices, 2, &count));
    CHECK(devices[0].direction == HA_CAPTURE && devices[1].direction == HA_PLAYBACK);
    OK(ha_device_probe(c, &d, HA_CAPTURE, &sc.capture_format, &probe));
    CHECK(probe.device_format_valid && probe.application_format.sample_rate == 48000);
    OK(ha_stream_plan(c, &sc, &sp)); OK(ha_stream_open(c, &sc, &s));
    CHECK(t.bytes == cp.bytes + sp.bytes && t.calls == 2);
    CHECK(ha_stream_open(c, &sc, &other) == HA_RESOURCE_EXHAUSTED && !other);
    CHECK(ha_context_destroy(&c) == HA_BUSY && c);
    close_stream(c, &s);
    old = devices[0].runtime_id;
    OK(ha_virtual_device_set(c, HA_CAPTURE, 0, 0, &id)); CHECK(!id);
    CHECK(ha_device_probe(c, &d, HA_CAPTURE, &sc.capture_format, &probe) == HA_DEVICE_UNAVAILABLE);
    OK(ha_virtual_device_set(c, HA_CAPTURE, 1, 0, &id)); CHECK(id != old);
    OK(ha_context_enumerate(c, devices, 2, &count)); CHECK(count == 2);
    CHECK(ha_stream_open(c, &sc, &s) == HA_PERMISSION_DENIED && !s);
    OK(ha_virtual_device_set(c, HA_CAPTURE, 1, 1, &id));
    sc.capture_device.kind = HA_SELECT_RUNTIME_ID; sc.capture_device.runtime_id = old;
    CHECK(ha_stream_open(c, &sc, &s) == HA_DEVICE_UNAVAILABLE);
    sc.capture_device.runtime_id = id; OK(ha_stream_open(c, &sc, &s)); close_stream(c, &s);
    OK(ha_context_get_status(c, &cs)); CHECK(cs.allocated_bytes == cp.bytes);
    OK(ha_context_destroy(&c)); OK(ha_context_destroy(&c)); CHECK(!t.live && !t.bytes);
    cfg.memory_limit = cp.bytes - 1u;
    CHECK(ha_context_create(&cfg, &c) == HA_RESOURCE_EXHAUSTED && !c);
    cfg.memory_limit = cp.bytes + sp.bytes - 1u;
    OK(ha_context_create(&cfg, &c));
    sc.capture_device.kind = HA_SELECT_DEFAULT;
    CHECK(ha_stream_open(c, &sc, &s) == HA_RESOURCE_EXHAUSTED); OK(ha_context_destroy(&c));
}

static void duplex_flush_and_flow(void)
{
    tracker t = {0}; ha_context *c; ha_stream *s;
    ha_context_config cfg = context_cfg(&t); ha_stream_config sc = stream_cfg(HA_CAPTURE | HA_PLAYBACK);
    ha_block_info b = HA_INIT(ha_block_info), ref = HA_INIT(ha_block_info);
    int16_t input[12], output[4], capture[4], reference[4];
    uint32_t n, i, calls; uint64_t generation;
    ha_virtual_config vc = HA_INIT(ha_virtual_config);
    for (i = 0; i < 12; ++i) input[i] = (int16_t)(i + 1u);
    OK(ha_context_create(&cfg, &c)); OK(ha_stream_open(c, &sc, &s));
    OK(ha_stream_write(s, input, 8, 1, &n)); CHECK(n == 8);
    CHECK(ha_stream_write(s, input, 1, 1, &n) == HA_WOULD_BLOCK && !n);
    start(c, s); calls = t.calls;
    step(s, 4, 4, output, &b); CHECK(output[0] == 1 && output[3] == 4);
    OK(ha_stream_read(s, capture, 2, &n, &b)); CHECK(n == 2 && capture[0] == -32768);
    OK(ha_stream_read(s, capture, 4, &n, &b)); CHECK(n == 2 && b.first_frame == 2 && capture[1] == -32765);
    OK(ha_stream_read_reference(s, reference, 4, &n, &ref)); CHECK(n == 4 && reference[3] == 4);
    vc.flush_delay_polls = 2; OK(ha_virtual_configure(s, &vc));
    CHECK(ha_stream_flush_playback(s, 0, &generation) == HA_PENDING && generation == 2);
    CHECK(ha_stream_write(s, input, 1, 1, &n) == HA_BUSY && !n);
    step(s, 4, 4, output, &b); CHECK(output[0] == 0 && b.generation == 2);
    poll_context(c); poll_context(c); CHECK(status(s).flush_result == HA_PENDING);
    poll_context(c); CHECK(status(s).flush_result == HA_OK && status(s).flush_completed_generation == 2);
    CHECK(ha_stream_write(s, input, 1, 1, &n) == HA_STALE_GENERATION && !n);
    OK(ha_stream_write(s, input, 4, generation, &n)); CHECK(n == 4);
    step(s, 4, 4, output, &b); CHECK(output[0] == 1);
    step(s, 4, 4, output, &b); CHECK(output[0] == 0 && (b.flags & HA_BLOCK_SILENCE));
    CHECK(status(s).playback_underrun_frames == 4);
    CHECK(status(s).capture_lost_frames == 4); /* unread positions 4..11 filled two slots */
    CHECK(status(s).reference_lost_frames > 0);
    OK(ha_stream_read(s, capture, 4, &n, &b)); CHECK(b.first_frame == 4);
    OK(ha_stream_read(s, capture, 4, &n, &b)); CHECK(b.first_frame == 8);
    step(s, 4, 0, NULL, &b);
    OK(ha_stream_read(s, capture, 4, &n, &b));
    CHECK(b.first_frame == 16 && b.lost_frames == 4 && (b.flags & HA_BLOCK_GAP));
    CHECK(t.calls == calls);
    CHECK(ha_stream_stop(s) == HA_PENDING); poll_context(c); CHECK(status(s).state == HA_STOPPED);
    CHECK(status(s).playback_generation == 3);
    start(c, s); CHECK(status(s).capture_epoch == 2);
    step(s, 4, 4, output, &b); CHECK(output[0] == 0);
    close_stream(c, &s); OK(ha_context_destroy(&c)); CHECK(!t.live);
}

static void faults_and_retirement(void)
{
    tracker t = {0}; ha_context *c; ha_stream *s;
    ha_context_config cfg = context_cfg(&t); ha_stream_config sc = stream_cfg(3);
    ha_virtual_config vc = HA_INIT(ha_virtual_config);
    uint64_t id, gen; unsigned fault;
    OK(ha_context_create(&cfg, &c));
    for (fault = 0; fault < 4; ++fault) {
        OK(ha_stream_open(c, &sc, &s));
        if (fault == 0) vc.faults = HA_VIRTUAL_FAIL_START;
        if (fault == 1) vc.faults = HA_VIRTUAL_PERMISSION_DENIED;
        if (fault == 2) vc.faults = HA_VIRTUAL_FAIL_STOP;
        if (fault == 3) vc.faults = HA_VIRTUAL_FAIL_FLUSH;
        OK(ha_virtual_configure(s, &vc));
        CHECK(ha_stream_start(s) == HA_PENDING); poll_context(c);
        if (fault == 2) { CHECK(ha_stream_stop(s) == HA_PENDING); poll_context(c); }
        if (fault == 3) { CHECK(ha_stream_flush_playback(s, 1, &gen) == HA_PENDING); poll_context(c); }
        CHECK(status(s).state == HA_FAILED && status(s).terminal_result < 0);
        CHECK(ha_stream_start(s) == HA_INVALID_STATE); close_stream(c, &s);
    }
    vc.faults = HA_VIRTUAL_HOLD_RETIREMENT;
    OK(ha_stream_open(c, &sc, &s)); OK(ha_virtual_configure(s, &vc)); start(c, s);
    CHECK(ha_stream_close_begin(s) == HA_PENDING); poll_context(c);
    CHECK(ha_stream_close_finish(&s) == HA_PENDING && s && t.live == 2);
    vc.faults = 0; OK(ha_virtual_configure(s, &vc)); poll_context(c);
    OK(ha_stream_close_finish(&s)); CHECK(!s);
    OK(ha_stream_open(c, &sc, &s)); start(c, s);
    OK(ha_virtual_device_set(c, HA_PLAYBACK, 0, 1, &id));
    CHECK(status(s).terminal_result == HA_DEVICE_LOST && status(s).state == HA_FAILED);
    close_stream(c, &s);
    OK(ha_virtual_device_set(c, HA_PLAYBACK, 1, 1, &id));
    OK(ha_stream_open(c, &sc, &s)); CHECK(status(s).playback_generation == 1); close_stream(c, &s);
    OK(ha_context_destroy(&c)); CHECK(!t.live);
}

static void gate_schedules_and_wrap(void)
{
    tracker t = {0}; ha_context *c; ha_stream *s;
    ha_context_config cfg = context_cfg(&t); ha_stream_config sc = stream_cfg(3);
    ha_block_info b = HA_INIT(ha_block_info);
    uint64_t generation; uint32_t n; int16_t pcm[4] = {7,8,9,10}, out[4];
    OK(ha_context_create(&cfg, &c)); OK(ha_stream_open(c, &sc, &s));
    /* Force counter rollover with a legal bounded queue distance. */
    atomic_store(&s->capture.read_index, UINT_MAX - 1u); atomic_store(&s->capture.write_index, UINT_MAX - 1u);
    atomic_store(&s->playback.read_index, UINT_MAX - 1u); atomic_store(&s->playback.write_index, UINT_MAX - 1u);
    OK(ha_stream_write(s, pcm, 4, 1, &n)); start(c, s);
    step(s, 4, 4, out, &b); CHECK(out[3] == 10);
    OK(ha_stream_read(s, out, 4, &n, &b));
    step(s, 4, 4, out, &b); OK(ha_stream_read(s, out, 4, &n, &b)); CHECK(b.first_frame == 4);
    atomic_store(&s->write_gate, HAIL_GATE_ACTIVE);
    CHECK(ha_stream_flush_playback(s, 0, &generation) == HA_BUSY);
    CHECK(status(s).playback_generation == 1);
    atomic_store(&s->write_gate, HAIL_GATE_IDLE);
    atomic_store(&s->read_gate, HAIL_GATE_ACTIVE);
    CHECK(ha_stream_stop(s) == HA_PENDING); poll_context(c);
    CHECK(status(s).state == HA_STOPPING);
    atomic_store_explicit(&s->read_gate, HAIL_GATE_IDLE, memory_order_release);
    poll_context(c); CHECK(status(s).state == HA_STOPPED);
    CHECK(ha_stream_flush_playback(s, 0, &generation) == HA_PENDING);
    poll_context(c); CHECK(status(s).flush_result == HA_OK); /* no callback needed */
    close_stream(c, &s); OK(ha_context_destroy(&c));
}

static void timing_formats_and_validation(void)
{
    static const uint32_t rates[] = {16000, 24000, 44100, 48000};
    tracker t = {0}; ha_context *c; ha_stream *s;
    ha_context_config cfg = context_cfg(&t); ha_stream_config sc;
    ha_virtual_config vc = HA_INIT(ha_virtual_config);
    ha_virtual_step_info vs = HA_INIT(ha_virtual_step_info);
    ha_block_info b = HA_INIT(ha_block_info), capture = HA_INIT(ha_block_info);
    ha_stream_info info = HA_INIT(ha_stream_info);
    ha_context_status cs = HA_INIT(ha_context_status);
    unsigned ri, channels, format, i; uint32_t n; unsigned char buf[64];
    uint64_t previous, epoch;
    cfg.event_capacity = 1; OK(ha_context_create(&cfg, &c));
    for (ri = 0; ri < 4; ++ri) for (channels = 1; channels <= 2; ++channels)
        for (format = HA_PCM_S16; format <= HA_PCM_F32; ++format) {
            ha_format f = HA_FORMAT_INIT(rates[ri], channels, format);
            sc = stream_cfg(3); sc.capture_format = f; sc.playback_format = f;
            OK(ha_stream_open(c, &sc, &s)); OK(ha_stream_get_info(s, &info));
            CHECK(info.capture_format.sample_rate == rates[ri] && info.clock_relationship == HA_CLOCK_INDEPENDENT);
            CHECK(info.capture_clock_domain != info.playback_clock_domain);
            vc.capture_drift_ppm = 1000; vc.playback_drift_ppm = -1000;
            OK(ha_virtual_configure(s, &vc)); start(c, s);
            previous = 0;
            for (i = 0; i < 10; ++i) {
                step(s, 4, 4, buf, &b);
                OK(ha_stream_read(s, buf, 4, &n, &capture)); CHECK(n == 4);
                CHECK(capture.first_frame == i * 4 && capture.host_time_ns >= previous);
                CHECK(capture.host_time_ns == (uint64_t)i * 4 * UINT64_C(1000000000000000) /
                    ((uint64_t)rates[ri] * 1001000));
                CHECK(b.host_time_ns == (uint64_t)i * 4 * UINT64_C(1000000000000000) /
                    ((uint64_t)rates[ri] * 999000));
                previous = capture.host_time_ns;
            }
            epoch = capture.clock_epoch;
            vs.flags = HA_VIRTUAL_UNKNOWN_CAPTURE_GAP; vs.capture_frames = 4;
            OK(ha_virtual_step(s, &vs, NULL, 0, &b));
            OK(ha_stream_read(s, buf, 4, &n, &capture));
            CHECK(capture.clock_epoch == epoch + 1 && (capture.flags & HA_BLOCK_UNKNOWN_GAP));
            close_stream(c, &s);
        }
    OK(ha_context_get_status(c, &cs)); CHECK(cs.lost_events && (cs.flags & HA_EVENT_RESCAN_REQUIRED));
    OK(ha_context_refresh(c));
    sc = stream_cfg(3); sc.capture_format.sample_rate = UINT32_MAX;
    CHECK(ha_stream_open(c, &sc, &s) == HA_FORMAT_UNSUPPORTED);
    sc = stream_cfg(3); sc.capture_slots = 3; CHECK(ha_stream_open(c, &sc, &s) == HA_INVALID_ARGUMENT);
    sc = stream_cfg(3); sc.quantum_frames = UINT32_MAX; CHECK(ha_stream_open(c, &sc, &s) == HA_INVALID_ARGUMENT);
    sc = stream_cfg(3); sc.capture_format.storage = 2; CHECK(ha_stream_open(c, &sc, &s) == HA_FORMAT_UNSUPPORTED);
    sc = stream_cfg(3); OK(ha_stream_open(c, &sc, &s)); start(c, s);
    vs.flags = 0; vs.capture_frames = 5; CHECK(ha_virtual_step(s, &vs, NULL, 0, &b) == HA_INVALID_ARGUMENT);
    CHECK(ha_stream_write(s, NULL, 1, 1, &n) == HA_INVALID_ARGUMENT && !n);
    OK(ha_stream_write(s, NULL, 0, 1, &n)); OK(ha_stream_read(s, NULL, 0, &n, &b));
    s->generation = UINT64_MAX;
    CHECK(ha_stream_flush_playback(s, 0, &epoch) == HA_OVERFLOW);
    close_stream(c, &s); OK(ha_context_destroy(&c)); CHECK(!t.live);
}

static void allocation_failures(void)
{
    unsigned fail;
    for (fail = 1; fail <= 3; ++fail) {
        tracker t = {0}; ha_context *c = NULL; ha_stream *s = NULL;
        ha_context_config cfg = context_cfg(&t); ha_stream_config sc = stream_cfg(3);
        t.fail = fail;
        if (fail == 1) CHECK(ha_context_create(&cfg, &c) == HA_RESOURCE_EXHAUSTED && !c);
        else {
            OK(ha_context_create(&cfg, &c));
            if (fail == 2) CHECK(ha_stream_open(c, &sc, &s) == HA_RESOURCE_EXHAUSTED && !s);
            else { OK(ha_stream_open(c, &sc, &s)); close_stream(c, &s); }
            OK(ha_context_destroy(&c));
        }
        CHECK(!t.live && !t.bytes);
    }
}

static void channel_bounds(void)
{
    tracker t={0}; ha_context *c=NULL; ha_stream *s=NULL;
    ha_context_config cc=context_cfg(&t); ha_stream_config sc=stream_cfg(HA_CAPTURE);
    ha_memory_plan p=HA_INIT(ha_memory_plan);
    OK(ha_context_create(&cc,&c));
    sc.capture_format.channels=0;
    CHECK(ha_stream_plan(c,&sc,&p)==HA_FORMAT_UNSUPPORTED);
    sc.capture_format.channels=HA_MAX_CHANNELS+1u; sc.capture_format.channel_layout=HA_LAYOUT_DISCRETE;
    CHECK(ha_stream_plan(c,&sc,&p)==HA_FORMAT_UNSUPPORTED);
    sc.capture_format.channels=4; sc.capture_format.channel_layout=HA_LAYOUT_STEREO;
    CHECK(ha_stream_plan(c,&sc,&p)==HA_FORMAT_UNSUPPORTED);
    sc.capture_format.channel_layout=HA_LAYOUT_DISCRETE; sc.capture_format.storage=HA_PLANAR;
    CHECK(ha_stream_plan(c,&sc,&p)==HA_FORMAT_UNSUPPORTED);
    sc.capture_format.storage=HA_INTERLEAVED; OK(ha_stream_open(c,&sc,&s));
    close_stream(c,&s);
    {
        float pcm[HA_MAX_CHANNELS]; uint32_t frames; ha_block_info b=HA_INIT(ha_block_info);
        sc.capture_format.channels=HA_MAX_CHANNELS; sc.capture_format.sample_format=HA_PCM_F32;
        sc.quantum_frames=HA_MAX_QUANTUM; OK(ha_stream_open(c,&sc,&s)); start(c,s);
        step(s,HA_MAX_QUANTUM,0,NULL,&b); OK(ha_stream_read(s,pcm,1,&frames,&b)); CHECK(frames==1);
        for(uint32_t ch=0;ch<HA_MAX_CHANNELS;++ch) CHECK(pcm[ch]==(float)(-32768+(int32_t)ch*17)/32768.0f);
        close_stream(c,&s);
    }
    OK(ha_context_destroy(&c)); CHECK(!t.live);
}

static void defaults_independence_and_partial_clock(void)
{
    tracker t = {0}; ha_context *c; ha_stream *a, *b;
    ha_context_config cfg = context_cfg(&t); ha_stream_config sc = stream_cfg(3);
    ha_stream_info ia = HA_INIT(ha_stream_info), ib = HA_INIT(ha_stream_info);
    ha_virtual_config vc = HA_INIT(ha_virtual_config);
    ha_block_info info = HA_INIT(ha_block_info);
    ha_virtual_step_info vs = HA_INIT(ha_virtual_step_info);
    int16_t pcm[HA_MAX_QUANTUM]; uint64_t id, old, gen; uint32_t i, n;
    cfg.max_devices = 3; cfg.max_streams = 2;
    OK(ha_context_create(&cfg, &c));
    sc.quantum_frames = HA_MAX_QUANTUM;
    OK(ha_stream_open(c, &sc, &a)); OK(ha_stream_get_info(a, &ia));
    old = ia.playback_device_id;
    OK(ha_virtual_device_add(c, HA_PLAYBACK, 1, &id));
    CHECK(ha_virtual_device_add(c, HA_CAPTURE, 1, &gen) == HA_RESOURCE_EXHAUSTED);
    OK(ha_virtual_default_set(c, HA_PLAYBACK, id));
    OK(ha_stream_open(c, &sc, &b)); OK(ha_stream_get_info(b, &ib));
    CHECK(ib.playback_device_id == id && ia.playback_device_id == old && status(a).state == HA_STOPPED);
    CHECK(ha_stream_open(c, &sc, &(ha_stream *){0}) == HA_RESOURCE_EXHAUSTED);
    vc.capture_drift_ppm = 1000; OK(ha_virtual_configure(a, &vc));
    start(c, a); start(c, b);
    vs.capture_frames = HA_MAX_QUANTUM;
    OK(ha_virtual_step(a, &vs, NULL, 0, &info));
    /* Partial-read interpolation must be monotonic even near a long slot's end
     * with device drift; a nominal-rate approximation regresses at this edge. */
    OK(ha_stream_read(a, pcm, HA_MAX_QUANTUM - 1u, &n, &info));
    CHECK(n == HA_MAX_QUANTUM - 1u);
    OK(ha_stream_read(a, pcm, 1, &n, &info));
    CHECK(info.first_frame == HA_MAX_QUANTUM - 1u);
    CHECK(info.host_time_ns == (uint64_t)(HA_MAX_QUANTUM - 1u) * UINT64_C(1000000000000000) /
        (UINT64_C(48000) * 1001000));
    gen = info.host_time_ns;
    vs.capture_frames = 1; OK(ha_virtual_step(a, &vs, NULL, 0, &info));
    OK(ha_stream_read(a, pcm, 1, &n, &info)); CHECK(info.host_time_ns > gen);
    CHECK(ha_stream_flush_playback(a, HA_FLUSH_NATIVE_REQUIRED, &gen) == HA_PENDING);
    poll_context(c); CHECK(status(a).capture_epoch == 1 && status(a).playback_epoch == 2);
    close_stream(c, &a); /* Stream b is unaffected by a's native reset and close. */
    vs.capture_frames = 4; OK(ha_virtual_step(b, &vs, NULL, 0, &info));
    OK(ha_stream_read(b, pcm, 4, &n, &info)); CHECK(n == 4 && info.first_frame == 0);
    for (i = 0; i < n; ++i) CHECK(pcm[i] == (int16_t)((int32_t)i - 32768));
    close_stream(c, &b); OK(ha_context_destroy(&c)); CHECK(!t.live);
}

static void lifecycle_and_output_guards(void)
{
    tracker t = {0}; ha_context *c; ha_stream *s;
    ha_context_config cfg = context_cfg(&t); ha_stream_config sc = stream_cfg(HA_CAPTURE);
    ha_virtual_config vc = HA_INIT(ha_virtual_config);
    ha_block_info block = HA_INIT(ha_block_info);
    ha_event event = HA_INIT(ha_event);
    ha_diagnostic d = HA_INIT(ha_diagnostic);
    ha_virtual_step_info step_info = HA_INIT(ha_virtual_step_info);
    uint32_t n; uint64_t gen;
    int16_t pcm[4] = {0};
    OK(ha_context_create(&cfg, &c)); sc.diagnostic = &d;
    sc.capture_format.sample_rate = 0;
    CHECK(ha_stream_open(c, &sc, &s) == HA_FORMAT_UNSUPPORTED && d.result == HA_FORMAT_UNSUPPORTED);
    sc.capture_format.sample_rate = 48000; OK(ha_stream_open(c, &sc, &s)); CHECK(d.result == HA_OK);
    CHECK(ha_stream_close_finish(&s) == HA_INVALID_STATE);
    CHECK(ha_stream_flush_playback(s, 0, &gen) == HA_UNSUPPORTED);
    CHECK(ha_stream_write(s, pcm, 1, 1, &n) == HA_UNSUPPORTED);
    CHECK(ha_stream_read_reference(s, pcm, 4, &n, &block) == HA_UNSUPPORTED);
    CHECK(ha_stream_read(s, pcm, 4, &n, &block) == HA_INVALID_STATE && !n);
    vc.start_delay_polls = 2; vc.stop_delay_polls = 2; OK(ha_virtual_configure(s, &vc));
    CHECK(ha_stream_start(s) == HA_PENDING); CHECK(ha_stream_start(s) == HA_INVALID_STATE);
    poll_context(c); CHECK(status(s).state == HA_STARTING);
    CHECK(ha_stream_stop(s) == HA_PENDING); CHECK(ha_stream_stop(s) == HA_PENDING);
    poll_context(c); poll_context(c); CHECK(status(s).state == HA_STOPPING);
    poll_context(c); CHECK(status(s).state == HA_STOPPED);
    OK(ha_stream_stop(s));
    vc.start_delay_polls = 0; vc.stop_delay_polls = 0; OK(ha_virtual_configure(s, &vc)); start(c, s);
    block.struct_size = 8;
    step_info.capture_frames = 4;
    CHECK(ha_virtual_step(s, &step_info, NULL, 0, &block) == HA_INVALID_ARGUMENT);
    block.struct_size = sizeof(block);
    OK(ha_virtual_step(s, &step_info, NULL, 0, &block));
    OK(ha_stream_read(s, pcm, 4, &n, &block)); CHECK(block.first_frame == 0);
    CHECK(ha_context_poll(c, &event, HA_MAX_EVENTS + 1u, &n) == HA_INVALID_ARGUMENT);
    event.struct_version = 2;
    CHECK(ha_context_poll(c, &event, 1, &n) == HA_INVALID_ARGUMENT);
    close_stream(c, &s); OK(ha_context_destroy(&c));
}

int main(void)
{
    discovery_and_limits(); duplex_flush_and_flow(); faults_and_retirement();
    gate_schedules_and_wrap(); timing_formats_and_validation(); allocation_failures();
    defaults_independence_and_partial_clock(); lifecycle_and_output_guards(); channel_bounds();
    puts("portable conformance passed"); return 0;
}
