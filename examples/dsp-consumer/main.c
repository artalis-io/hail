/* Only this scenario driver knows the PUBLIC virtual device API. consumer.c
 * uses hail.h alone and can consume the same records from a native stream. */
#include "consumer.h"
#include <hail/virtual.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define OK(x) CHECK((x) == HA_OK)
typedef struct allocation { unsigned calls, live; } allocation;
static void *HA_CALL allocate(void *user, size_t bytes, size_t alignment)
{
    allocation *a = user;
    void *base = malloc(bytes + alignment + sizeof(void *)); uintptr_t p;
    CHECK(base); p = ((uintptr_t)base + sizeof(void *) + alignment - 1u) & ~(uintptr_t)(alignment - 1u);
    ((void **)p)[-1] = base; ++a->calls; ++a->live; return (void *)p;
}
static void HA_CALL release(void *user, void *ptr, size_t bytes, size_t alignment)
{ allocation *a = user; (void)bytes; CHECK((uintptr_t)ptr % alignment == 0 && a->live); --a->live; free(((void **)ptr)[-1]); }
static void poll_context(ha_context *c)
{ uint32_t n; OK(ha_context_poll(c, NULL, 0, &n)); }
static ha_stream_status status(ha_stream *s)
{ ha_stream_status st = HA_INIT(ha_stream_status); OK(ha_stream_get_status(s, &st)); return st; }
static void drain(dsp_consumer *d)
{
    dsp_output out;
    while (dsp_take(d, &out)) for (uint32_t i = 0; i < out.info.frames; ++i)
        CHECK(out.mono[i] == (float)((out.info.first_frame + i) % 256u) / 1024.0f);
}
static void step(ha_stream *s, dsp_consumer *d, uint64_t *position, uint32_t capture,
                 uint32_t render, uint32_t flags, int write_audio, int pump, int take)
{
    float input[DSP_QUANTUM * HA_MAX_CHANNELS], output[DSP_QUANTUM * HA_MAX_CHANNELS];
    ha_virtual_step_info v = HA_INIT(ha_virtual_step_info);
    ha_block_info b = HA_INIT(ha_block_info); uint32_t i, ch, n;
    const uint32_t cc = d->format.capture_format.channels, pc = d->format.playback_format.channels;
    float value = d->generation == 1 ? 0.25f : 0.5f;
    for (i = 0; i < capture; ++i) for (ch = 0; ch < cc; ++ch)
        input[(size_t)i * cc + ch] = (float)((*position + i) % 256u) / 512.0f + (float)ch / 128.0f;
    if (write_audio && render) {
        for (i = 0; i < render * pc; ++i) output[i] = value;
        OK(ha_stream_write(s, output, render, d->generation, &n)); CHECK(n == render);
    }
    v.capture_frames = capture; v.playback_frames = render; v.flags = flags;
    v.capture_data = input; v.advance_ns = UINT64_C(1000000);
    OK(ha_virtual_step(s, &v, output, render, &b)); *position += capture;
    if (write_audio) for (i = 0; i < render * pc; ++i) CHECK(output[i] == value);
    if (pump) {
        OK(dsp_pump(d, s));
        if (d->reference_info.frames && d->reference_info.generation == d->generation && write_audio)
            CHECK(d->reference_pcm[0] == value);
    }
    if (take) drain(d);
}
static void close_stream(ha_context *c, ha_stream **s)
{ CHECK(ha_stream_close_begin(*s) == HA_PENDING); poll_context(c); OK(ha_stream_close_finish(s)); CHECK(!*s); }

static void scenario(uint32_t rate, uint32_t capture_channels, uint32_t render_channels,
                     int32_t ppm, unsigned iterations)
{
    allocation a = {0}; ha_context *c = NULL; ha_stream *s = NULL;
    ha_context_config cc = HA_INIT(ha_context_config);
    ha_stream_config cfg = HA_INIT(ha_stream_config);
    ha_device_selector selector = HA_INIT(ha_device_selector);
    ha_format cf = HA_FORMAT_INIT(rate, capture_channels, HA_PCM_F32);
    ha_format pf = HA_FORMAT_INIT(48000, render_channels, HA_PCM_F32);
    ha_stream_info info = HA_INIT(ha_stream_info);
    ha_virtual_config clocks = HA_INIT(ha_virtual_config);
    dsp_consumer d; uint64_t position = 0, rendered = 0, next, epoch, old; uint32_t n;
    float stale[DSP_QUANTUM * HA_MAX_CHANNELS] = {0};
    cc.backend = HA_BACKEND_VIRTUAL; cc.event_capacity = 2;
    cc.allocator = (ha_allocator)HA_INIT(ha_allocator);
    cc.allocator.allocate = allocate; cc.allocator.deallocate = release; cc.allocator.user = &a;
    cfg.directions = HA_CAPTURE | HA_PLAYBACK;
    cfg.capture_device = selector; cfg.playback_device = selector;
    cfg.capture_format = cf; cfg.playback_format = pf;
    cfg.quantum_frames = DSP_QUANTUM; cfg.capture_slots = cfg.playback_slots = cfg.reference_slots = 2;
    OK(ha_context_create(&cc, &c)); OK(ha_stream_open(c, &cfg, &s));
    OK(ha_stream_get_info(s, &info)); OK(dsp_init(&d, &info));
    CHECK(info.stream_id != 0);
    {
        ha_event events[2]={HA_INIT(ha_event),HA_INIT(ha_event)}; uint32_t count;
        ha_stream_info legacy=HA_INIT(ha_stream_info);
        legacy.struct_size=(uint32_t)offsetof(ha_stream_info,stream_id); legacy.stream_id=UINT64_MAX;
        OK(ha_stream_get_info(s,&legacy)); CHECK(legacy.stream_id==UINT64_MAX);
        CHECK(legacy.capture_clock_domain==info.capture_clock_domain);
        /* Drain discovery notifications before observing this stream's event. */
        do { OK(ha_context_poll(c,events,2,&count)); } while(count);
    }
    CHECK(info.capture_clock_domain != info.playback_clock_domain);
    CHECK(info.capture_format.channels == capture_channels);
    clocks.capture_drift_ppm = ppm; clocks.playback_drift_ppm = -ppm; clocks.host_time_ns = UINT64_C(1000000000);
    clocks.flush_delay_polls = 2;
    OK(ha_virtual_configure(s, &clocks)); CHECK(ha_stream_start(s) == HA_PENDING); poll_context(c);
    {
        ha_event event=HA_INIT(ha_event); uint32_t count;
        OK(ha_context_poll(c,&event,1,&count));
        CHECK(count==1 && event.stream_id==info.stream_id && event.type==HA_EVENT_STATE);
    }
    for (unsigned i = 0; i < iterations; ++i) {
        uint32_t count = 16u + (i % 4u) * 8u;
        uint32_t demand = rate == 48000 ? count : count * 3u;
        step(s, &d, &position, count, demand, 0, 1, 1, 1); rendered += demand;
    }
    CHECK(d.capture.frames == position && d.render.frames == rendered);
    CHECK(d.capture.observed_ppm > (double)ppm - 0.1 && d.capture.observed_ppm < (double)ppm + 0.1);
    CHECK(d.render.observed_ppm > (double)-ppm - 0.1 && d.render.observed_ppm < (double)-ppm + 0.1);
    CHECK(!status(s).capture_lost_frames && !status(s).playback_underrun_frames);
    /* DSP output backpressure: consumer stops reading capture when its two
     * output blocks fill; reference keeps progressing independently. */
    for (unsigned i = 0; i < 8; ++i) step(s, &d, &position, 16, 48, 0, 1, 1, 0);
    CHECK(d.count == DSP_SLOTS && status(s).capture_lost_frames > 0); drain(&d);
    for (unsigned i = 0; i < 4; ++i) { OK(dsp_pump(&d, s)); drain(&d); }
    step(s, &d, &position, 16, 48, 0, 1, 1, 1);
    CHECK(d.capture.lost && d.capture.gaps);
    /* Reference overflow never stalls playback; silence is observable. */
    for (unsigned i = 0; i < 5; ++i) step(s, &d, &position, 0, 48, 0, 0, 0, 0);
    CHECK(status(s).reference_lost_frames && status(s).playback_underrun_frames);
    for (unsigned i = 0; i < 3; ++i) OK(dsp_pump(&d, s));
    step(s, &d, &position, 16, 48, 0, 0, 1, 1); CHECK(d.selected_silence_frames);
    /* Queued old selection records remain tagged; new writes cannot revive
     * canceled application work. Capture advances through delayed flush. */
    old = d.generation; epoch = d.capture.epoch;
    step(s, &d, &position, 0, 48, 0, 1, 0, 0);
    step(s, &d, &position, 0, 48, 0, 1, 0, 0);
    CHECK(ha_stream_flush_playback(s, HA_FLUSH_NATIVE_REQUIRED, &next) == HA_PENDING);
    CHECK(ha_stream_write(s, stale, 1, old, &n) == HA_BUSY && !n);
    CHECK(ha_stream_flush_playback(s, 0, &old) == HA_BUSY);
    step(s, &d, &position, 16, 0, 0, 0, 0, 0);
    poll_context(c); CHECK(status(s).flush_result == HA_PENDING);
    poll_context(c); CHECK(status(s).flush_result == HA_PENDING); poll_context(c);
    CHECK(status(s).flush_completed_generation == next && status(s).flush_result == HA_OK);
    CHECK(ha_stream_write(s, stale, 1, d.generation, &n) == HA_STALE_GENERATION && !n);
    dsp_cancelled(&d, next); OK(dsp_pump(&d, s)); drain(&d);
    step(s, &d, &position, 16, 48, 0, 1, 1, 1); OK(dsp_pump(&d, s));
    CHECK(d.obsolete_reference_frames == 96);
    CHECK(d.capture.epoch == epoch && d.render.epoch > 1);
    step(s, &d, &position, 16, 48, HA_VIRTUAL_UNKNOWN_CAPTURE_GAP, 1, 1, 1);
    CHECK(d.capture.epoch > epoch && d.capture.gaps);
    /* Repeated stop/start revokes queues and creates fresh per-leg epochs. */
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(ha_stream_stop(s) == HA_PENDING); poll_context(c); CHECK(status(s).state == HA_STOPPED);
        dsp_cancelled(&d, status(s).playback_generation); position = 0;
        CHECK(ha_stream_start(s) == HA_PENDING); poll_context(c);
        step(s, &d, &position, 16, 48, 0, 1, 1, 1);
    }
    CHECK(a.calls == 2); close_stream(c, &s);
    /* Reopen at a new rate does not reuse clock-domain identities. */
    cfg.capture_format.sample_rate = rate == 16000 ? 24000 : 16000;
    OK(ha_stream_open(c, &cfg, &s)); OK(ha_stream_get_info(s, &info));
    CHECK(info.capture_clock_domain != d.format.capture_clock_domain);
    OK(dsp_init(&d,&info)); position=0;
    CHECK(ha_stream_start(s)==HA_PENDING); poll_context(c);
    step(s,&d,&position,16,48,0,1,1,1);
    {
        uint64_t replacement, removed;
        ha_stream_info pinned=HA_INIT(ha_stream_info);
        OK(ha_virtual_device_add(c,HA_CAPTURE,1,&replacement));
        OK(ha_virtual_default_set(c,HA_CAPTURE,replacement));
        OK(ha_stream_get_info(s,&pinned)); CHECK(pinned.capture_device_id==info.capture_device_id);
        OK(ha_virtual_default_set(c,HA_CAPTURE,info.capture_device_id));
        OK(ha_virtual_device_set(c,HA_CAPTURE,0,1,&removed));
        CHECK(status(s).state==HA_FAILED && status(s).terminal_result==HA_DEVICE_LOST);
        close_stream(c,&s);
        OK(ha_virtual_device_set(c,HA_CAPTURE,1,0,&replacement));
        CHECK(replacement!=info.capture_device_id);
        CHECK(ha_stream_open(c,&cfg,&s)==HA_PERMISSION_DENIED && !s);
        OK(ha_virtual_device_set(c,HA_CAPTURE,1,1,&replacement));
    }
    OK(ha_stream_open(c,&cfg,&s));
    clocks.faults=HA_VIRTUAL_HOLD_RETIREMENT;
    OK(ha_virtual_configure(s,&clocks));
    CHECK(ha_stream_close_begin(s)==HA_PENDING); poll_context(c);
    CHECK(ha_stream_close_finish(&s)==HA_PENDING && s);
    clocks.faults=0; OK(ha_virtual_configure(s,&clocks));
    poll_context(c); OK(ha_stream_close_finish(&s));
    OK(ha_context_destroy(&c)); CHECK(!a.live);
    printf("PASS public DSP rate=%u capture_channels=%u render_channels=%u drift=%d steps=%u simulated_capture_seconds=%.3f\n",
           rate, capture_channels, render_channels, ppm, iterations, (double)iterations * 28.0 / (double)rate);
}
int main(void)
{
    scenario(48000, 1, 2, 0, 4000);
    scenario(16000, 4, 2, 1000, 4000);
    scenario(44100, HA_MAX_CHANNELS, 8, -1000, 4000);
    scenario(16000, 4, 2, 100, 400000); /* 700 seconds modeled; no wall-time audio. */
    return 0;
}
