#define _POSIX_C_SOURCE 200809L
#include <hail.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

#define Q 256u
#define CODES 65536u
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static uint64_t sent[2][CODES], latencies[131072];
static uint64_t compared, latency_count, capture_frames, capture_after, zeros, bad, gaps, reported_loss;
static uint64_t allocations, frees, allocated, peak, last_frame, capture_epoch;
static uint32_t last_code, last_phase, seen, new_seen;
static ha_context *context;
static ha_stream *stream;
static uint64_t now_ns(void)
{ struct timespec t; CHECK(!clock_gettime(CLOCK_MONOTONIC, &t)); return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec; }
static void pause_control(void) { struct timespec t = {0,500000}; nanosleep(&t, NULL); }
static void *HA_CALL allocate(void *user, size_t bytes, size_t alignment)
{ void *p = NULL; (void)user; if (posix_memalign(&p, alignment, bytes)) return NULL; ++allocations; allocated += bytes; if (allocated > peak) peak = allocated; return p; }
static void HA_CALL deallocate(void *user, void *p, size_t bytes, size_t alignment)
{ (void)user; (void)alignment; ++frees; allocated -= bytes; free(p); }
static void progress(void)
{ uint32_t n; CHECK(ha_context_poll(context, NULL, 0, &n) == HA_OK); }
static ha_stream_status status(void)
{ ha_stream_status s = HA_INIT(ha_stream_status); CHECK(ha_stream_get_status(stream, &s) == HA_OK); CHECK(s.state != HA_FAILED); return s; }
static void capture(uint32_t after_flush)
{
    float pcm[Q * 2u]; uint32_t n, i, attempts;
    for (attempts = 0; attempts < 16; ++attempts) {
        ha_block_info block = HA_INIT(ha_block_info); ha_result r = ha_stream_read(stream, pcm, Q, &n, &block);
        uint64_t read_time = now_ns();
        if (r == HA_WOULD_BLOCK || r == HA_BUSY) break;
        CHECK(r == HA_OK);
        if (capture_frames && block.first_frame != last_frame) ++gaps;
        if (!capture_epoch) capture_epoch = block.clock_epoch;
        CHECK(block.clock_epoch == capture_epoch);
        last_frame = block.first_frame + n; capture_frames += n;
        if (block.lost_frames > reported_loss) reported_loss = block.lost_frames;
        if (after_flush) capture_after += n;
        for (i = 0; i < n; ++i) {
            float left = pcm[i * 2u], right = pcm[i * 2u + 1u];
            uint32_t encoded, phase, code;
            if (left == 0 && right == 0) { ++zeros; continue; }
            if (!(left >= 0.125f && left < 0.625f) || right != -left) { ++bad; continue; }
            encoded = (uint32_t)(left * 1048576.0f);
            if ((float)encoded / 1048576.0f != left) { ++bad; continue; }
            if (encoded >= 131072u && encoded < 131072u + CODES) phase = 0;
            else if (encoded >= 524288u && encoded < 524288u + CODES) phase = 1;
            else { ++bad; continue; }
            code = encoded - (phase ? 524288u : 131072u);
            if (phase == 0 && new_seen) ++bad;
            if (phase == 1) new_seen = 1;
            if (seen && phase == last_phase && code != ((last_code + 1u) & (CODES - 1u))) ++gaps;
            seen = 1; last_phase = phase; last_code = code; ++compared;
            if (sent[phase][code] && sent[phase][code] <= read_time && latency_count < sizeof(latencies) / sizeof(latencies[0]))
                latencies[latency_count++] = read_time - sent[phase][code];
        }
    }
}
static void produce(uint32_t phase, uint32_t *position, uint64_t generation)
{
    float pcm[Q * 2u]; uint32_t i, accepted; ha_result r; uint64_t before = now_ns();
    for (i = 0; i < Q; ++i) { uint32_t code = (*position + i) & (CODES - 1u); pcm[2u*i] = (float)((phase ? 524288u : 131072u) + code) / 1048576.0f; pcm[2u*i+1u] = -pcm[2u*i]; }
    r = ha_stream_write(stream, pcm, Q, generation, &accepted);
    CHECK(r == HA_OK || r == HA_WOULD_BLOCK || r == HA_BUSY);
    if (r == HA_OK) { for (i = 0; i < accepted; ++i) sent[phase][(*position + i) & (CODES - 1u)] = before; *position += accepted; }
}
static int compare_u64(const void *a, const void *b)
{ uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return (x > y) - (x < y); }
static double cpu_us(const struct rusage *r)
{ return (double)r->ru_utime.tv_sec * 1000000.0 + r->ru_utime.tv_usec + (double)r->ru_stime.tv_sec * 1000000.0 + r->ru_stime.tv_usec; }
int main(void)
{
    ha_context_config cc = HA_INIT(ha_context_config); ha_allocator allocator = HA_INIT(ha_allocator);
    ha_stream_config sc = HA_INIT(ha_stream_config); ha_device_selector select = HA_INIT(ha_device_selector);
    ha_device_info devices[HA_MAX_DEVICES]; ha_format format = HA_FORMAT_INIT(48000,2,HA_PCM_F32);
    ha_stream_status state; struct rusage usage_before, usage_after; char command[32];
    uint32_t i, count, position = 0, accepted; uint64_t generation = 1, start, deadline, flush_start, flush_ack, old_capture, steady_allocations, finish;
    uint64_t capture_id = 0, playback_id = 0; float stale[2] = {0};
    CHECK(getenv("HA_ISOLATED_PIPEWIRE") && !strcmp(getenv("HA_ISOLATED_PIPEWIRE"), "1"));
    allocator.allocate = allocate; allocator.deallocate = deallocate; cc.allocator = allocator; cc.backend = HA_BACKEND_PIPEWIRE;
    CHECK(ha_context_create(&cc, &context) == HA_OK);
    for (i = 0; i < HA_MAX_DEVICES; ++i) { ha_device_info d = HA_INIT(ha_device_info); devices[i] = d; }
    CHECK(ha_context_enumerate(context, devices, HA_MAX_DEVICES, &count) == HA_OK && count == 2);
    for (i = 0; i < count; ++i) {
        if (devices[i].direction == HA_CAPTURE && !strcmp(devices[i].name,"Monitor of Hail isolated loopback")) capture_id = devices[i].runtime_id;
        if (devices[i].direction == HA_PLAYBACK && !strcmp(devices[i].name,"Hail isolated loopback")) playback_id = devices[i].runtime_id;
    }
    CHECK(capture_id && playback_id);
    select.kind = HA_SELECT_RUNTIME_ID; select.runtime_id = capture_id; sc.capture_device = select;
    select.runtime_id = playback_id; sc.playback_device = select; sc.directions = HA_CAPTURE | HA_PLAYBACK;
    sc.capture_format = format; sc.playback_format = format; sc.negotiation = HA_EXACT_APPLICATION;
    sc.quantum_frames = Q; sc.capture_slots = 16; sc.playback_slots = 4;
    CHECK(ha_stream_open(context, &sc, &stream) == HA_OK);
    puts("READY"); fflush(stdout); CHECK(fgets(command, sizeof(command), stdin) && !strcmp(command,"GO\n"));
    CHECK(ha_stream_start(stream) == HA_PENDING); deadline = now_ns() + 3000000000u;
    do { progress(); state = status(); CHECK(now_ns() < deadline); pause_control(); } while (state.state == HA_STARTING);
    CHECK(state.state == HA_RUNNING); steady_allocations = allocations; CHECK(!getrusage(RUSAGE_SELF, &usage_before));
    start = now_ns(); deadline = start + 1000000000u;
    while (now_ns() < deadline) { progress(); capture(0); produce(0,&position,generation); pause_control(); }
    CHECK(compared >= 16000);
    flush_start = now_ns(); old_capture = capture_frames;
    CHECK(ha_stream_flush_playback(stream, HA_FLUSH_NATIVE_REQUIRED, &generation) == HA_PENDING);
    deadline = now_ns() + 3000000000u;
    for (;;) {
        progress(); capture(1); state = status();
        if (state.flush_completed_generation == generation) { flush_ack = now_ns(); break; }
        CHECK(now_ns() < deadline); pause_control();
    }
    CHECK(state.flush_result == HA_OK);
    CHECK(ha_stream_write(stream, stale, 1, generation - 1u, &accepted) == HA_STALE_GENERATION);
    position = 0; deadline = now_ns() + 1000000000u;
    while (now_ns() < deadline) { progress(); capture(1); produce(1,&position,generation); pause_control(); }
    CHECK(new_seen && capture_frames > old_capture && capture_after && !bad && !gaps);
    finish = now_ns(); CHECK(!getrusage(RUSAGE_SELF, &usage_after)); state = status(); CHECK(!state.capture_lost_frames);
    CHECK(allocations == steady_allocations);
    qsort(latencies,(size_t)latency_count,sizeof(latencies[0]),compare_u64); CHECK(latency_count);
    printf("{\"compared_frames\":%llu,\"capture_frames\":%llu,\"capture_after_flush\":%llu,\"zero_frames\":%llu,\"bad_samples\":%llu,\"sequence_gaps\":%llu,\"capture_epoch\":%llu,\"flush_ack_us\":%.3f,\"producer_to_capture_us\":{\"median\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f},\"elapsed_ms\":%.3f,\"cpu_ms\":%.3f,\"peak_rss_kib\":%ld,\"hail_peak_allocated_bytes\":%llu,\"steady_hail_allocations\":0,\"underrun_frames\":%llu}\n",
      (unsigned long long)compared,(unsigned long long)capture_frames,(unsigned long long)capture_after,(unsigned long long)zeros,(unsigned long long)bad,(unsigned long long)gaps,(unsigned long long)capture_epoch,(double)(flush_ack-flush_start)/1000.0,
      (double)latencies[latency_count/2u]/1000.0,(double)latencies[latency_count*95u/100u]/1000.0,(double)latencies[latency_count*99u/100u]/1000.0,(double)latencies[latency_count-1u]/1000.0,
      (double)(finish-start)/1000000.0,(cpu_us(&usage_after)-cpu_us(&usage_before))/1000.0,usage_after.ru_maxrss,(unsigned long long)peak,(unsigned long long)state.playback_underrun_frames);
    puts("REMOVE"); fflush(stdout); CHECK(fgets(command,sizeof(command),stdin) && !strcmp(command,"REMOVED\n"));
    CHECK(ha_context_refresh(context) == HA_OK); CHECK(ha_stream_get_status(stream,&state) == HA_OK);
    CHECK(state.state == HA_FAILED && state.terminal_result == HA_DEVICE_LOST);
    CHECK(ha_stream_close_begin(stream) == HA_PENDING); deadline = now_ns() + 3000000000u;
    while (stream) { progress(); if (ha_stream_close_finish(&stream) == HA_OK) break; CHECK(now_ns()<deadline); pause_control(); }
    puts("REPLACE"); fflush(stdout); CHECK(fgets(command,sizeof(command),stdin) && !strcmp(command,"REPLACED\n"));
    CHECK(ha_context_refresh(context) == HA_OK);
    CHECK(ha_context_enumerate(context,devices,HA_MAX_DEVICES,&count) == HA_OK && count==2);
    for (i=0;i<count;++i) {
        CHECK(devices[i].runtime_id != capture_id && devices[i].runtime_id != playback_id);
        if (devices[i].direction == HA_CAPTURE) sc.capture_device.runtime_id = devices[i].runtime_id;
        else sc.playback_device.runtime_id = devices[i].runtime_id;
    }
    CHECK(ha_stream_open(context, &sc, &stream) == HA_OK);
    puts("REOPEN"); fflush(stdout); CHECK(fgets(command,sizeof(command),stdin) && !strcmp(command,"GO\n"));
    capture_frames = 0; capture_epoch = 0; last_frame = 0; compared = 0; seen = 0; new_seen = 0; gaps = 0; bad = 0;
    position = 0; generation = 1; steady_allocations = allocations;
    CHECK(ha_stream_start(stream) == HA_PENDING); deadline = now_ns() + 3000000000u;
    do { progress(); state = status(); CHECK(now_ns() < deadline); pause_control(); } while (state.state == HA_STARTING);
    CHECK(state.state == HA_RUNNING); deadline = now_ns() + 250000000u;
    while (now_ns() < deadline) { progress(); capture(0); produce(1,&position,generation); pause_control(); }
    CHECK(compared > 4096 && !gaps && !bad && allocations == steady_allocations);
    old_capture = capture_frames; state = status();
    {
        uint64_t before_underrun = state.playback_underrun_frames;
        struct timespec stall = {0,200000000};
        deadline = now_ns() + 100000000u;
        while (now_ns() < deadline) { progress(); capture(0); pause_control(); }
        state = status(); CHECK(state.playback_underrun_frames > before_underrun && capture_frames > old_capture);
        nanosleep(&stall, NULL); /* Deliberately fill and overrun the bounded capture queue. */
        deadline = now_ns() + 1000000000u;
        do { progress(); capture(0); state = status(); CHECK(now_ns() < deadline); pause_control(); }
        while (!state.capture_lost_frames || !reported_loss || !gaps);
        CHECK(!bad && allocations == steady_allocations);
        printf("XRUN {\"capture_lost_frames\":%llu,\"block_reported_loss\":%llu,\"playback_underrun_frames\":%llu}\n",
            (unsigned long long)state.capture_lost_frames, (unsigned long long)reported_loss,
            (unsigned long long)state.playback_underrun_frames);
        fflush(stdout);
    }
    CHECK(ha_stream_close_begin(stream) == HA_PENDING); deadline = now_ns() + 3000000000u;
    while (stream) { progress(); if (ha_stream_close_finish(&stream) == HA_OK) break; CHECK(now_ns()<deadline); pause_control(); }
    CHECK(ha_context_destroy(&context) == HA_OK); CHECK(allocated==0 && allocations==frees);
    puts("PASS sample accuracy, capture-preserving flush, stale writes, device loss/replacement, explicit reopen and teardown");
    return 0;
}
