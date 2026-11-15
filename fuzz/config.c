#include <hail.h>
#include <hail/virtual.h>
#include <stdlib.h>
#include <string.h>

typedef struct arena { _Alignas(16) unsigned char bytes[131072]; size_t used; } arena;
static void *HA_CALL arena_alloc(void *user, size_t size, size_t alignment)
{
    arena *a = user;
    size_t at = (a->used + alignment - 1u) & ~(alignment - 1u);
    if (at > sizeof(a->bytes) || size > sizeof(a->bytes) - at) return NULL;
    a->used = at + size; return a->bytes + at;
}
static void HA_CALL arena_free(void *user, void *p, size_t size, size_t alignment)
{
    (void)user; (void)p; (void)size; (void)alignment;
}
static uint32_t word(const uint8_t *data, size_t size, size_t index)
{
    uint32_t v = 0;
    if (index <= size && size - index >= sizeof(v)) memcpy(&v, data + index, sizeof(v));
    return v;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    arena a = {0}; ha_context *c = NULL; ha_stream *s = NULL;
    ha_context_config cc = HA_INIT(ha_context_config);
    ha_stream_config sc = HA_INIT(ha_stream_config);
    ha_device_selector selector = HA_INIT(ha_device_selector);
    ha_format f = HA_FORMAT_INIT(48000, 1, HA_PCM_S16);
    ha_virtual_step_info step = HA_INIT(ha_virtual_step_info);
    ha_virtual_config vc = HA_INIT(ha_virtual_config);
    ha_block_info block = HA_INIT(ha_block_info);
    unsigned char pcm[65536] = {0};
    uint32_t n = 0, command = word(data, size, 0), i;
    uint64_t gen = 1;
    cc.backend = HA_BACKEND_VIRTUAL; cc.max_devices = 2; cc.max_streams = 1;
    cc.event_capacity = 1; cc.memory_limit = sizeof(a.bytes);
    cc.allocator.struct_size = sizeof(ha_allocator); cc.allocator.struct_version = 1;
    cc.allocator.allocate = arena_alloc; cc.allocator.deallocate = arena_free; cc.allocator.user = &a;
    if (ha_context_create(&cc, &c) != HA_OK) abort();
    sc.directions = (command & 1u) ? (command >> 8) : HA_CAPTURE | HA_PLAYBACK;
    sc.capture_device = selector; sc.playback_device = selector;
    sc.capture_format = f; sc.playback_format = f;
    sc.quantum_frames = (command & 2u) ? word(data, size, 4) : 32;
    sc.capture_slots = 2; sc.playback_slots = 2; sc.reference_slots = 2;
    if (command & 4u) sc.capture_format.sample_rate = word(data, size, 8);
    if (command & 8u) sc.playback_format.channels = word(data, size, 12);
    if (command & 16u) sc.capture_slots = word(data, size, 16);
    if (command & 32u) sc.capture_format.struct_size = word(data, size, 20);
    if (ha_stream_open(c, &sc, &s) == HA_OK) {
        vc.capture_drift_ppm = (int32_t)(word(data, size, 24) % 2001u) - 1000;
        vc.host_time_ns = (command & 64u) ? UINT64_MAX - word(data, size, 28) : 0;
        (void)ha_virtual_configure(s, &vc);
        (void)ha_stream_start(s); (void)ha_context_poll(c, NULL, 0, &n);
        for (i = 0; i < 8; ++i) {
            uint32_t op = word(data, size, 32u + i * 4u);
            step.capture_frames = op & 16383u;
            step.playback_frames = (op >> 14) & 16383u;
            step.flags = op >> 28;
            (void)ha_virtual_step(s, &step, pcm, HA_MAX_QUANTUM, &block);
            (void)ha_stream_read(s, pcm, op & 16383u, &n, &block);
            if (n > HA_MAX_QUANTUM) abort();
            (void)ha_stream_write(s, pcm, (op >> 14) & 16383u, gen, &n);
            if (n > HA_MAX_QUANTUM) abort();
            if (op & 1u) (void)ha_stream_flush_playback(s, (op >> 1) & 3u, &gen);
            (void)ha_context_poll(c, NULL, 0, &n);
        }
        (void)ha_stream_close_begin(s); (void)ha_context_poll(c, NULL, 0, &n);
        if (ha_stream_close_finish(&s) != HA_OK) abort();
    }
    if (ha_context_destroy(&c) != HA_OK) abort();
    return 0;
}
