#define _POSIX_C_SOURCE 200809L
#include <hail.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static void pause_control(void) { Sleep(2); }
#else
#include <time.h>
static void pause_control(void) { struct timespec t = {0,2000000}; nanosleep(&t, NULL); }
#endif

/* Enumerates by default. Explicit IDs enable a short in-memory capture and/or
 * silent playback exercise; no recording is persisted and no sound is emitted.
 * Usage: hail_native_example BACKEND [CAPTURE_ID PLAYBACK_ID]
 * Zero means disabled. BACKEND: 0 auto, 2 CoreAudio, 3 WASAPI, 4 PipeWire, 5 ALSA.
 */
int main(int argc, char **argv)
{
    ha_context_config cc = HA_INIT(ha_context_config); ha_context *c = NULL; ha_stream *s = NULL;
    ha_device_info devices[HA_MAX_DEVICES]; ha_stream_config sc = HA_INIT(ha_stream_config);
    ha_stream_status status = HA_INIT(ha_stream_status); ha_block_info block = HA_INIT(ha_block_info);
    ha_device_selector selector = HA_INIT(ha_device_selector); ha_format format = HA_FORMAT_INIT(48000, 2, HA_PCM_F32);
    uint32_t i, count = 0; uint64_t capture = 0, playback = 0, generation = 1, frames = 0, after_flush = 0, capture_epoch = 0; int result = 1;
    float data[256 * 2] = {0}; ha_result r;
    cc.backend = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : HA_BACKEND_AUTO;
    r = ha_context_create(&cc, &c);
    if (r != HA_OK) { fprintf(stderr, "context: %s\n", ha_result_string(r)); return 1; }
    for (i = 0; i < HA_MAX_DEVICES; ++i) { ha_device_info d = HA_INIT(ha_device_info); devices[i] = d; }
    r = ha_context_enumerate(c, devices, HA_MAX_DEVICES, &count);
    if (r != HA_OK) goto cleanup;
    for (i = 0; i < count; ++i) printf("%llu %s %s%s\n", (unsigned long long)devices[i].runtime_id,
        devices[i].direction == HA_CAPTURE ? "capture" : "playback", devices[i].name, devices[i].is_default ? " [default]" : "");
    if (argc < 4) { result = 0; goto cleanup; }
    capture = strtoull(argv[2], NULL, 10); playback = strtoull(argv[3], NULL, 10);
    if (!capture && !playback) { result = 0; goto cleanup; }
    sc.directions = (capture ? HA_CAPTURE : 0u) | (playback ? HA_PLAYBACK : 0u); sc.negotiation = HA_EXACT_APPLICATION;
    selector.kind = HA_SELECT_RUNTIME_ID;
    selector.runtime_id = capture; sc.capture_device = selector;
    selector.runtime_id = playback; sc.playback_device = selector;
    sc.capture_format = format; sc.playback_format = format; sc.quantum_frames = 256;
    sc.capture_slots = capture ? 8u : 0u; sc.playback_slots = playback ? 8u : 0u;
    r = ha_stream_open(c, &sc, &s); if (r != HA_OK) goto cleanup;
    r = ha_stream_start(s); if (r != HA_PENDING) goto cleanup;
    for (i = 0; i < 1500; ++i) {
        (void)ha_context_poll(c, NULL, 0, &count); (void)ha_stream_get_status(s, &status);
        if (status.state == HA_FAILED) { r = status.terminal_result; goto cleanup; }
        if (status.state == HA_RUNNING) break;
        pause_control();
    }
    if (status.state != HA_RUNNING) { r = HA_STREAM_FAILED; goto cleanup; }
    capture_epoch = status.capture_epoch;
    for (i = 0; i < 150; ++i) {
        (void)ha_context_poll(c, NULL, 0, &count);
        if (capture) { if (ha_stream_read(s, data, 256, &count, &block) == HA_OK) { frames += count; if (i > 90) after_flush += count; } }
        memset(data, 0, sizeof(data));
        if (playback) (void)ha_stream_write(s, data, 256, generation, &count);
        if (playback && i == 75) {
            r = ha_stream_flush_playback(s, HA_FLUSH_NATIVE_REQUIRED, &generation);
            if (r != HA_PENDING) goto cleanup;
        }
        (void)ha_stream_get_status(s, &status);
        if (status.state == HA_FAILED) { r = status.terminal_result; goto cleanup; }
        pause_control();
    }
    printf("captured=%llu after_flush=%llu capture_epoch=%llu/%llu flush=%s generation=%llu\n", (unsigned long long)frames,
           (unsigned long long)after_flush, (unsigned long long)capture_epoch, (unsigned long long)status.capture_epoch,
           ha_result_string(status.flush_result), (unsigned long long)status.playback_generation);
    if ((capture && (!frames || !after_flush)) || (playback && status.flush_result != HA_OK)) { r = HA_STREAM_FAILED; goto cleanup; }
    result = 0; r = HA_OK;
cleanup:
    if (r != HA_OK) fprintf(stderr, "native exercise: %s\n", ha_result_string(r));
    if (s) {
        (void)ha_stream_close_begin(s);
        for (i = 0; i < 1500 && s; ++i) {
            (void)ha_context_poll(c, NULL, 0, &count);
            if (ha_stream_close_finish(&s) == HA_OK) break;
            pause_control();
        }
        if (s) { fprintf(stderr, "retirement still pending; storage retained\n"); return 1; }
    }
    for (i = 0; i < 1500 && c; ++i) {
        r = ha_context_destroy(&c);
        if (r == HA_OK) break;
        if (r != HA_PENDING) return 1;
        pause_control();
    }
    if (c) return 1;
    return result;
}
