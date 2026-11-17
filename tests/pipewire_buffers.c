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
static struct pw_buffer *test_dequeue(struct pw_stream *stream)
{ (void)stream; return next_buffer; }
static int test_queue(struct pw_stream *stream, struct pw_buffer *buffer)
{ (void)stream; (void)buffer; ++returned; return 0; }
static int test_time(struct pw_stream *stream, struct pw_time *time, size_t size)
{ (void)stream; (void)time; (void)size; return -ENOTSUP; }
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
int main(void)
{
    ha_context_config cc = HA_INIT(ha_context_config);
    ha_stream_config sc = HA_INIT(ha_stream_config);
    ha_device_selector selector = HA_INIT(ha_device_selector);
    ha_format format = HA_FORMAT_INIT(48000, 1, HA_PCM_S16);
    ha_context *c; ha_stream *s; uint32_t count;
    int16_t samples[] = {11, 22, 33, 44}, output[4];
    struct spa_chunk chunk = {0}; struct spa_data data = {0};
    struct spa_buffer buffer = {0}; struct pw_buffer pw = {0}; pw_hail_leg leg = {0};
    const int32_t strides[] = {-2, INT32_MIN, 1, 4, 0, 2};
    cc.backend = HA_BACKEND_VIRTUAL; CHECK(ha_context_create(&cc, &c) == HA_OK);
    sc.directions = HA_CAPTURE; sc.capture_device = selector; sc.capture_format = format; sc.quantum_frames = 4;
    CHECK(ha_stream_open(c, &sc, &s) == HA_OK);
    CHECK(ha_stream_start(s) == HA_PENDING); CHECK(ha_context_poll(c, NULL, 0, &count) == HA_OK);
    s->concurrent = 1; hail_rt_open(s, 0);
    leg.owner = s; leg.leg = 0; atomic_init(&leg.prepared, 1); atomic_init(&leg.callbacks, 0);
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
    atomic_store(&s->rt[0].error, 0); s->concurrent = 0;
    CHECK(ha_stream_close_begin(s) == HA_PENDING); CHECK(ha_context_poll(c, NULL, 0, &count) == HA_OK);
    CHECK(ha_stream_close_finish(&s) == HA_OK); CHECK(ha_context_destroy(&c) == HA_OK);
    puts("PipeWire capture rejects unsupported strides and returns every buffer lease");
    return 0;
}
