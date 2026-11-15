#include <hail.h>
#include <hail/virtual.h>
#include <atomic>
#include <thread>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)

static void *HA_CALL alloc(void *, size_t bytes, size_t alignment)
{
    void *base = std::malloc(bytes + alignment + sizeof(void *));
    if (!base) return nullptr;
    uintptr_t p = (reinterpret_cast<uintptr_t>(base) + sizeof(void *) + alignment - 1u) & ~(uintptr_t)(alignment - 1u);
    reinterpret_cast<void **>(p)[-1] = base;
    return reinterpret_cast<void *>(p);
}
static void HA_CALL release(void *, void *p, size_t, size_t)
{
    std::free(reinterpret_cast<void **>(p)[-1]);
}

int main()
{
    ha_context_config cc = HA_INIT(ha_context_config);
    ha_stream_config sc = HA_INIT(ha_stream_config);
    ha_format f = HA_FORMAT_INIT(48000, 1, HA_PCM_S16);
    ha_device_selector selector = HA_INIT(ha_device_selector);
    ha_context *c = nullptr; ha_stream *s = nullptr;
    std::atomic<bool> done(false), ready_reader(false), ready_writer(false), ready_reference(false);
    std::atomic<uint64_t> generation(1), reads(0), writes(0);
    cc.backend = HA_BACKEND_VIRTUAL;
    cc.allocator = HA_INIT(ha_allocator); cc.allocator.allocate = alloc; cc.allocator.deallocate = release;
    sc.directions = HA_CAPTURE | HA_PLAYBACK;
    sc.capture_device = selector; sc.playback_device = selector;
    sc.capture_format = f; sc.playback_format = f;
    sc.quantum_frames = 32; sc.capture_slots = 8; sc.playback_slots = 8; sc.reference_slots = 8;
    CHECK(ha_context_create(&cc, &c) == HA_OK); CHECK(ha_stream_open(c, &sc, &s) == HA_OK);
    CHECK(ha_stream_start(s) == HA_PENDING);
    uint32_t count;
    CHECK(ha_context_poll(c, nullptr, 0, &count) == HA_OK);
    std::thread reader([&] {
        int16_t pcm[32]; uint64_t last_end = 0, last_epoch = 0;
        ha_block_info info = HA_INIT(ha_block_info);
        ready_reader.store(true, std::memory_order_release);
        while (!done.load(std::memory_order_acquire)) {
            uint32_t n = 0; ha_result r = ha_stream_read(s, pcm, 32, &n, &info);
            if (r == HA_OK && n) {
                if (info.clock_epoch == last_epoch) CHECK(info.first_frame >= last_end);
                for (uint32_t i = 0; i < n; ++i)
                    CHECK(pcm[i] == (int16_t)((int32_t)((info.first_frame + i) & 65535u) - 32768));
                last_epoch = info.clock_epoch; last_end = info.first_frame + n;
                reads.fetch_add(n, std::memory_order_relaxed);
            } else CHECK(r == HA_WOULD_BLOCK || r == HA_BUSY || r == HA_INVALID_STATE);
            std::this_thread::yield();
        }
    });
    std::thread writer([&] {
        int16_t pcm[32]; for (auto &value : pcm) value = 1234;
        ready_writer.store(true, std::memory_order_release);
        while (!done.load(std::memory_order_acquire)) {
            uint32_t n = 0;
            ha_result r = ha_stream_write(s, pcm, 32, generation.load(std::memory_order_acquire), &n);
            CHECK(r == HA_OK || r == HA_WOULD_BLOCK || r == HA_BUSY ||
                  r == HA_STALE_GENERATION || r == HA_INVALID_STATE);
            writes.fetch_add(n, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });
    std::thread reference([&] {
        int16_t pcm[32]; ha_block_info info = HA_INIT(ha_block_info);
        ready_reference.store(true, std::memory_order_release);
        while (!done.load(std::memory_order_acquire)) {
            uint32_t n = 0; ha_result r = ha_stream_read_reference(s, pcm, 32, &n, &info);
            CHECK(r == HA_OK || r == HA_WOULD_BLOCK || r == HA_BUSY || r == HA_INVALID_STATE);
            for (uint32_t i = 0; i < n; ++i) CHECK(pcm[i] == 0 || pcm[i] == 1234);
            std::this_thread::yield();
        }
    });
    while (!ready_reader.load(std::memory_order_acquire) || !ready_writer.load(std::memory_order_acquire) ||
           !ready_reference.load(std::memory_order_acquire)) std::this_thread::yield();
    ha_virtual_step_info step = HA_INIT(ha_virtual_step_info);
    ha_block_info info = HA_INIT(ha_block_info);
    step.capture_frames = 32; step.playback_frames = 32;
    int16_t out[32];
    for (unsigned i = 0; i < 20000; ++i) {
        CHECK(ha_virtual_step(s, &step, out, 32, &info) == HA_OK);
        for (auto value : out) CHECK(value == 0 || value == 1234);
        if (i % 37u == 0) {
            uint64_t next = 0;
            ha_result r = ha_stream_flush_playback(s, 0, &next);
            CHECK(r == HA_PENDING || r == HA_BUSY);
            if (r == HA_PENDING) generation.store(next, std::memory_order_release);
        }
        CHECK(ha_context_poll(c, nullptr, 0, &count) == HA_OK);
        std::this_thread::yield();
    }
    /* Begin close while data roles are live; join before final destruction. */
    CHECK(ha_stream_close_begin(s) == HA_PENDING);
    CHECK(ha_context_poll(c, nullptr, 0, &count) == HA_OK);
    done.store(true, std::memory_order_release);
    reader.join(); writer.join(); reference.join();
    CHECK(ha_context_poll(c, nullptr, 0, &count) == HA_OK);
    CHECK(ha_stream_close_finish(&s) == HA_OK && !s);
    CHECK(ha_context_destroy(&c) == HA_OK);
    CHECK(reads.load() && writes.load());
    std::puts("concurrent reader/writer/reference/flush/close passed");
    return 0;
}
