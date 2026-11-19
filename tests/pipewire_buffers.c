/* Exercise the actual process callback against malformed native descriptors.
 * No PipeWire server, device, callback thread or timing sleeps are needed. */
#define _POSIX_C_SOURCE 200809L
#include <pipewire/pipewire.h>
#include <hail.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
static struct pw_buffer *next_buffer;
static unsigned returned;
static int time_valid;
static struct pw_time time_record;
static struct pw_buffer *test_dequeue(struct pw_stream *stream)
{ (void)stream; return next_buffer; }
static int test_queue(struct pw_stream *stream, struct pw_buffer *buffer)
{ (void)stream; (void)buffer; ++returned; return 0; }
static int test_time(struct pw_stream *stream, struct pw_time *time, size_t size)
{ (void)stream; (void)size; if(!time_valid) return -ENOTSUP; *time=time_record; return 0; }
#define pw_stream_dequeue_buffer test_dequeue
#define pw_stream_queue_buffer test_queue
#define pw_stream_get_time_n test_time
#define hail_pipewire_backend hail_test_pipewire_backend
#include "../src/backend_pipewire.c"
#undef hail_pipewire_backend
#undef pw_stream_get_time_n
#undef pw_stream_queue_buffer
#undef pw_stream_dequeue_buffer
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
int main(int argc, char **argv)
{
    ha_context_config cc = HA_INIT(ha_context_config);
    ha_stream_config sc = HA_INIT(ha_stream_config);
    ha_device_selector selector = HA_INIT(ha_device_selector);
    ha_format format = HA_FORMAT_INIT(48000, 1, HA_PCM_S16);
    ha_context *c; ha_stream *s; uint32_t count;
    int16_t samples[] = {11, 22, 33, 44}, output[4];
    unsigned char scratch[sizeof(samples)];
    struct spa_chunk chunk = {0}; struct spa_data data = {0};
    struct spa_buffer buffer = {0}; struct pw_buffer pw = {0}; pw_hail_leg leg = {0};
    const int32_t strides[] = {-2, INT32_MIN, 1, 4, 0, 2};
    cc.backend = HA_BACKEND_VIRTUAL; CHECK(ha_context_create(&cc, &c) == HA_OK);
    sc.directions = HA_CAPTURE|HA_PLAYBACK; sc.capture_device = selector; sc.capture_format = format; sc.quantum_frames = 4;
    sc.playback_device=selector; sc.playback_format=format; sc.reference_slots=2;
    CHECK(ha_stream_open(c, &sc, &s) == HA_OK);
    CHECK(ha_stream_start(s) == HA_PENDING); CHECK(ha_context_poll(c, NULL, 0, &count) == HA_OK);
    s->concurrent = 1; hail_rt_open(s, 0);
    leg.owner = s; leg.leg = 0; atomic_init(&leg.prepared, 1); atomic_init(&leg.callbacks, 0);
    leg.capture_scratch = scratch;
    data.data = samples; data.maxsize = sizeof(samples); data.chunk = &chunk;
    buffer.n_datas = 1; buffer.datas = &data; pw.buffer = &buffer; next_buffer = &pw;
    for (unsigned empty = 0; empty < 2; ++empty) for (unsigned i = 0; i < sizeof(strides) / sizeof(strides[0]); ++i) {
        ha_block_info info = HA_INIT(ha_block_info);
        unsigned before = returned; uint32_t frames = 99;
        atomic_store(&s->rt[0].error, 0);
        chunk.stride = strides[i]; chunk.size = empty ? 0u : (uint32_t)sizeof(samples);
        stream_process(&leg);
        CHECK(returned == before + 1 && !atomic_load(&leg.callbacks));
        if (i < 4) {
            CHECK(atomic_load(&s->rt[0].error) == HA_STREAM_FAILED);
            CHECK(ha_stream_read(s, output, 4, &frames, &info) == HA_WOULD_BLOCK && !frames);
        } else {
            CHECK(!atomic_load(&s->rt[0].error));
            CHECK(ha_stream_read(s, output, 4, &frames, &info) == (empty ? HA_WOULD_BLOCK : HA_OK));
            CHECK(frames == (empty ? 0u : 4u));
            if (!empty) CHECK(!memcmp(output, samples, sizeof(samples)));
        }
    }
    /* SPA offsets are logical ring offsets, not necessarily memory offsets. */
    if (argc < 2 || strcmp(argv[1], "neutral")) {
        ha_block_info info = HA_INIT(ha_block_info); uint32_t frames;
        const int16_t expected[] = {44, 11, 22, 33};
        atomic_store(&s->rt[0].error, 0);
        chunk.offset = 3u * sizeof(samples) + 6u; chunk.size = sizeof(samples); chunk.stride = 2;
        stream_process(&leg);
        CHECK(!atomic_load(&s->rt[0].error));
        CHECK(ha_stream_read(s, output, 4, &frames, &info) == HA_OK && frames == 4);
        CHECK(!memcmp(output, expected, sizeof(expected)));
        /* Clamp size before validation, including a byte-unaligned ring split. */
        chunk.offset = UINT32_MAX; chunk.size = UINT32_MAX;
        stream_process(&leg);
        CHECK(!atomic_load(&s->rt[0].error));
        CHECK(ha_stream_read(s, output, 4, &frames, &info) == HA_OK && frames == 4);
        for (unsigned i=0; i<sizeof(samples); ++i)
            CHECK(((unsigned char *)output)[i] == ((unsigned char *)samples)[(7u+i)%sizeof(samples)]);
    }
    chunk.offset = 0; chunk.size = sizeof(samples); chunk.stride = 2;
    for (unsigned kind = 0; kind < 2; ++kind) {
        struct spa_meta_header header = {0}; struct spa_meta meta = {0};
        ha_block_info info = HA_INIT(ha_block_info); uint32_t frames;
        meta.type = SPA_META_Header; meta.size = sizeof(header); meta.data = &header;
        buffer.n_metas = 1; buffer.metas = &meta;
        chunk.flags = kind ? 0 : SPA_CHUNK_FLAG_EMPTY;
        header.flags = kind ? SPA_META_HEADER_FLAG_GAP : 0;
        atomic_store(&s->rt[0].error, 0); stream_process(&leg);
        CHECK(!atomic_load(&s->rt[0].error));
        CHECK(ha_stream_read(s, output, 4, &frames, &info) == HA_OK && frames == 4);
        CHECK((info.flags & HA_BLOCK_SILENCE) && !output[0] && !output[1] && !output[2] && !output[3]);
        CHECK(samples[0] == 11 && samples[3] == 44); /* SDK storage is read-only to Hail. */
    }
    buffer.n_metas = 0; buffer.metas = NULL; chunk.flags = 0;
    {
        size_t bytes; uint32_t frames; ha_block_info info=HA_INIT(ha_block_info);
        ha_stream_info plan=HA_INIT(ha_stream_info);
        plan.capture_slots=2; plan.capture_format=format; plan.quantum_frames=4;
        CHECK(pipewire_extra_bytes(&plan,&bytes)==HA_OK && bytes==sizeof(samples));
        /* A corrupt packet advances loss/epoch; the next actual payload carries it. */
        chunk.flags=SPA_CHUNK_FLAG_CORRUPTED; stream_process(&leg); chunk.flags=0;
        stream_process(&leg);
        CHECK(ha_stream_read(s,output,4,&frames,&info)==HA_OK && frames==4);
        CHECK((info.flags & (HA_BLOCK_GAP|HA_BLOCK_UNKNOWN_GAP|HA_BLOCK_DISCONTINUITY))==
              (HA_BLOCK_GAP|HA_BLOCK_UNKNOWN_GAP|HA_BLOCK_DISCONTINUITY));
        CHECK(info.lost_frames==4 && info.uncertainty_ns==UINT64_MAX && !(info.flags&HA_TIME_HOST_VALID));
    }
    atomic_store(&s->rt[0].error, 0);
    leg.leg=1; hail_rt_open(s,1); time_valid=1;
    time_record.now=INT64_C(10000000000); time_record.rate.num=1; time_record.rate.denom=48000;
    {
        ha_block_info info=HA_INIT(ha_block_info); uint32_t frames;
        const int16_t signal[]={101,202,303,404};
        CHECK(ha_stream_write(s,signal,4,1,&frames)==HA_OK && frames==4);
        chunk.flags=SPA_CHUNK_FLAG_EMPTY; stream_process(&leg);
        CHECK(!chunk.flags && !memcmp(samples,signal,sizeof(signal)));
        CHECK(ha_stream_read_reference(s,output,4,&frames,&info)==HA_OK && frames==4);
        CHECK(!(info.flags&HA_BLOCK_SILENCE) && !memcmp(output,signal,sizeof(signal)));
    }
    for(unsigned invalid=0;invalid<3;++invalid) {
        ha_block_info info=HA_INIT(ha_block_info); uint32_t frames;
        time_record.queued=invalid==1?UINT64_MAX:0;
        time_record.buffered=invalid==2?UINT64_MAX:0;
        stream_process(&leg);
        CHECK(ha_stream_read_reference(s,output,4,&frames,&info)==HA_OK && frames==4);
        CHECK(((info.flags & HA_TIME_HOST_VALID)!=0)==(invalid==0));
    }
    s->concurrent = 0;
    CHECK(ha_stream_close_begin(s) == HA_PENDING); CHECK(ha_context_poll(c, NULL, 0, &count) == HA_OK);
    CHECK(ha_stream_close_finish(&s) == HA_OK); CHECK(ha_context_destroy(&c) == HA_OK);
    puts("PipeWire ring offsets, neutral media, strides and time validity verified");
    return 0;
}
