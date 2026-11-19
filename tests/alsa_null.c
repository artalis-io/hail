#define _POSIX_C_SOURCE 200809L
#include <hail.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
static void tick(ha_context *c)
{
    uint32_t count; struct timespec pause = {0,1000000};
    CHECK(ha_context_poll(c, NULL, 0, &count) == HA_OK); nanosleep(&pause, NULL);
}
static ha_stream_status status(ha_stream *s)
{ ha_stream_status value = HA_INIT(ha_stream_status); CHECK(ha_stream_get_status(s, &value) == HA_OK); return value; }
int main(int argc, char **argv)
{
    ha_context_config cc = HA_INIT(ha_context_config); ha_context *c = NULL; ha_stream *s = NULL;
    ha_stream_config sc = HA_INIT(ha_stream_config); ha_device_selector select = HA_INIT(ha_device_selector);
    ha_device_info devices[HA_MAX_DEVICES]; ha_format format = HA_FORMAT_INIT(48000, 2, HA_PCM_F32);
    ha_block_info block = HA_INIT(ha_block_info); ha_stream_status state;
    float pcm[64u * HA_MAX_CHANNELS] = {0}; uint32_t i, count, n, found = 0; uint64_t generation = 1, captured = 0, after = 0, capture_epoch;
    CHECK(argc==1 || argc==2);
    if(argc==2) {
        char *end; unsigned long channels=strtoul(argv[1],&end,10);
        CHECK(end!=argv[1] && !*end && channels>=1 && channels<=HA_MAX_CHANNELS);
        format.channels=(uint32_t)channels;
        format.channel_layout=channels==1?HA_LAYOUT_MONO:channels==2?HA_LAYOUT_STEREO:HA_LAYOUT_DISCRETE;
    }
    cc.backend = HA_BACKEND_ALSA;
    if (ha_context_create(&cc, &c) != HA_OK) return 77;
    for (i = 0; i < HA_MAX_DEVICES; ++i) { ha_device_info value = HA_INIT(ha_device_info); devices[i] = value; }
    CHECK(ha_context_enumerate(c, devices, HA_MAX_DEVICES, &count) == HA_OK);
    for (i = 0; i < count; ++i) if (devices[i].persistent_key_bytes == 4 && !memcmp(devices[i].persistent_key, "null", 4)) ++found;
    if (found != 2) { CHECK(ha_context_destroy(&c) == HA_OK); return 77; }
    select.kind = HA_SELECT_PERSISTENT_KEY; select.key_bytes = 4; memcpy(select.key, "null", 5);
    sc.capture_device = select; sc.playback_device = select; sc.capture_format = format; sc.playback_format = format;
    sc.directions = HA_CAPTURE | HA_PLAYBACK; sc.negotiation = HA_EXACT_END_TO_END;
    sc.quantum_frames = 64; sc.capture_slots = 4; sc.playback_slots = 4; sc.reference_slots = 4;
    CHECK(ha_stream_open(c, &sc, &s) == HA_OK);
    CHECK(ha_stream_start(s) == HA_PENDING);
    for (i = 0; i < 1000 && status(s).state == HA_STARTING; ++i) tick(c);
    state = status(s); CHECK(state.state == HA_RUNNING); capture_epoch = state.capture_epoch;
    for (i = 0; i < 150; ++i) {
        ha_result r;
        tick(c);
        r = ha_stream_read(s, pcm, 64, &n, &block);
        CHECK(r == HA_OK || r == HA_WOULD_BLOCK);
        if (r == HA_OK) { uint32_t j; captured += n; if (i > 80) after += n; for (j = 0; j < n * format.channels; ++j) CHECK(pcm[j] == 0); }
        memset(pcm, 0, sizeof(pcm));
        r = ha_stream_write(s, pcm, 64, generation, &n);
        CHECK(r == HA_OK || r == HA_WOULD_BLOCK || r == HA_BUSY);
        if (i == 75) CHECK(ha_stream_flush_playback(s, HA_FLUSH_NATIVE_REQUIRED, &generation) == HA_PENDING);
        state = status(s); CHECK(state.state == HA_RUNNING && state.capture_epoch == capture_epoch);
    }
    CHECK(captured && after && state.flush_result == HA_OK && state.playback_epoch == 2);
    CHECK(ha_stream_stop(s) == HA_PENDING);
    for (i = 0; i < 1000 && status(s).state == HA_STOPPING; ++i) tick(c);
    CHECK(status(s).state == HA_STOPPED);
    CHECK(ha_stream_write(s, pcm, 64, generation, &n) == HA_STALE_GENERATION);
    CHECK(ha_stream_start(s) == HA_PENDING); tick(c); CHECK(status(s).state == HA_RUNNING);
    CHECK(ha_stream_close_begin(s) == HA_PENDING);
    for (i = 0; i < 1000 && s; ++i) { tick(c); if (ha_stream_close_finish(&s) == HA_OK) break; }
    CHECK(!s); CHECK(ha_context_destroy(&c) == HA_OK);
    printf("ALSA null channels=%u duplex, persistent selection, flush/capture continuity and restart passed\n",format.channels); return 0;
}
