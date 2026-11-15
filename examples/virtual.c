#include <hail.h>
#include <hail/virtual.h>
#include <stdio.h>

/* A fixed arena supplies all Hail storage; no heap or threads are required.
 * This example also works with HA_FREESTANDING=ON (stdio is application-only). */
typedef struct arena { _Alignas(16) unsigned char bytes[65536]; size_t used; } arena;
static void *HA_CALL arena_allocate(void *user, size_t bytes, size_t alignment)
{
    arena *a = user;
    size_t start = (a->used + alignment - 1u) & ~(alignment - 1u);
    if (start > sizeof(a->bytes) || bytes > sizeof(a->bytes) - start) return NULL;
    a->used = start + bytes; return a->bytes + start;
}
static void HA_CALL arena_free(void *user, void *ptr, size_t bytes, size_t alignment)
{
    (void)user; (void)ptr; (void)bytes; (void)alignment;
    /* One-shot arena is reused only after context destruction. */
}

int main(void)
{
    arena memory = {0};
    ha_context_config cc = HA_INIT(ha_context_config);
    ha_stream_config sc = HA_INIT(ha_stream_config);
    ha_format f = HA_FORMAT_INIT(48000, 1, HA_PCM_S16);
    ha_device_selector selector = HA_INIT(ha_device_selector);
    ha_device_info devices[2] = { HA_INIT(ha_device_info), HA_INIT(ha_device_info) };
    ha_virtual_step_info step = HA_INIT(ha_virtual_step_info);
    ha_block_info block = HA_INIT(ha_block_info);
    ha_stream_status state = HA_INIT(ha_stream_status);
    ha_context *context = NULL; ha_stream *stream = NULL;
    int16_t pcm[4] = {100,200,300,400}, playback[4], capture[4];
    uint32_t count, accepted; uint64_t generation;
    int result = 1;
#define REQUIRE(call, expected) do { ha_result r_ = (call); if (r_ != (expected)) { \
    fprintf(stderr, "%s: %s\n", #call, ha_result_string(r_)); goto cleanup; } } while (0)
    cc.backend = HA_BACKEND_VIRTUAL; cc.max_streams = 1; cc.event_capacity = 8;
    cc.allocator.struct_size = sizeof(ha_allocator); cc.allocator.struct_version = 1;
    cc.allocator.allocate = arena_allocate; cc.allocator.deallocate = arena_free; cc.allocator.user = &memory;
    REQUIRE(ha_context_create(&cc, &context), HA_OK);
    REQUIRE(ha_context_enumerate(context, devices, 2, &count), HA_OK);
    printf("%s; %s\n", devices[0].name, devices[1].name);
    sc.directions = HA_CAPTURE | HA_PLAYBACK; sc.capture_device = selector; sc.playback_device = selector;
    sc.capture_format = f; sc.playback_format = f; sc.quantum_frames = 4;
    sc.capture_slots = 2; sc.playback_slots = 2; sc.reference_slots = 2;
    REQUIRE(ha_stream_open(context, &sc, &stream), HA_OK);
    REQUIRE(ha_stream_write(stream, pcm, 4, 1, &accepted), HA_OK);
    REQUIRE(ha_stream_start(stream), HA_PENDING);
    REQUIRE(ha_context_poll(context, NULL, 0, &count), HA_OK);
    step.capture_frames = 4; step.playback_frames = 4;
    REQUIRE(ha_virtual_step(stream, &step, playback, 4, &block), HA_OK);
    REQUIRE(ha_stream_read(stream, capture, 4, &count, &block), HA_OK);
    printf("Captured %u frames at %llu ns; playback first sample %d\n",
           count, (unsigned long long)block.host_time_ns, playback[0]);
    REQUIRE(ha_stream_flush_playback(stream, 0, &generation), HA_PENDING);
    REQUIRE(ha_context_poll(context, NULL, 0, &count), HA_OK);
    REQUIRE(ha_stream_write(stream, pcm, 4, 1, &accepted), HA_STALE_GENERATION);
    REQUIRE(ha_virtual_step(stream, &step, playback, 4, &block), HA_OK);
    REQUIRE(ha_stream_read(stream, capture, 4, &count, &block), HA_OK);
    printf("Capture continued at frame %llu after interruption\n", (unsigned long long)block.first_frame);
    {
        uint64_t id;
        REQUIRE(ha_virtual_device_set(context, HA_CAPTURE, 0, 0, &id), HA_OK);
        REQUIRE(ha_stream_get_status(stream, &state), HA_OK);
        if (state.terminal_result != HA_DEVICE_LOST) goto cleanup;
    }
    result = 0;
cleanup:
    if (stream) {
        (void)ha_stream_close_begin(stream);
        (void)ha_context_poll(context, NULL, 0, &count);
        if (ha_stream_close_finish(&stream) != HA_OK) result = 1;
    }
    if (ha_context_destroy(&context) != HA_OK) result = 1;
    return result;
}
