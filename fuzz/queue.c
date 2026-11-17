#include "support.h"
/* Independent slot FIFO oracle: no production cursor/mapping helpers. */
typedef struct model_slot { unsigned char pcm[512]; ha_block_info info; uint32_t offset; } model_slot;
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fuzz_input in = {data, size, 0}; hail_ring r = {0};
    _Alignas(16) unsigned char storage[8 * (sizeof(hail_slot) + 512 + 16)];
    model_slot model[8]; unsigned head = 0, count = 0;
    unsigned char input[1024], actual[1024];
    r.slots = 1u << (byte(&in) % 4u); r.quantum = 1u + byte(&in) % 64u;
    r.bytes_per_frame = 2u << (byte(&in) % 3u);
    r.stride = (uint32_t)((sizeof(hail_slot) + r.quantum * r.bytes_per_frame + 15u) / 16u * 16u);
    r.storage = storage;
    unsigned initial = UINT32_MAX - byte(&in);
    atomic_init(&r.read_index, initial); atomic_init(&r.write_index, initial);
    for (unsigned op = 0; op < 256 && in.at < in.size; ++op) {
        unsigned action = byte(&in); uint32_t frames = byte(&in) % 129u;
        if (action % 8u == 0) { hail_ring_reset(&r); head = count = 0; continue; }
        if (action & 1u) {
            ha_block_info info = HA_INIT(ha_block_info);
            info.first_frame = word(&in); info.device_position = word(&in);
            info.clock_epoch = byte(&in); info.generation = byte(&in);
            info.host_time_ns = (action & 2u) ? UINT64_MAX - byte(&in) : word(&in);
            info.frame_time_numerator = UINT64_C(1000000000);
            info.frame_time_denominator = (action & 4u) ? 44100 : 48000;
            info.time_remainder = byte(&in); info.flags = HA_TIME_HOST_VALID | HA_TIME_DEVICE_VALID |
                HA_BLOCK_GAP | HA_BLOCK_DISCONTINUITY | HA_BLOCK_UNKNOWN_GAP;
            for (size_t j = 0; j < sizeof(input); ++j) input[j] = (unsigned char)(j + action + op);
            uint32_t expected = 0;
            while (expected < frames && count < r.slots) {
                uint32_t n = frames - expected; if (n > r.quantum) n = r.quantum;
                model_slot *slot = &model[(head + count) % r.slots];
                slot->info = info; slot->info.first_frame += expected; slot->info.device_position += expected;
                slot->info.frames = n; slot->offset = 0;
                memcpy(slot->pcm, input + expected * r.bytes_per_frame, n * r.bytes_per_frame);
                ++count; expected += n;
            }
            CHECK(hail_ring_push(&r, input, frames, &info) == expected);
        } else {
            ha_block_info info = HA_INIT(ha_block_info); uint32_t expected = 0;
            memset(actual, 0xa5, sizeof(actual));
            if (count && frames) {
                model_slot *slot = &model[head]; expected = slot->info.frames - slot->offset;
                if (expected > frames) expected = frames;
                CHECK(hail_ring_read(&r, actual, frames, &info) == expected);
                CHECK(!memcmp(actual, slot->pcm + slot->offset * r.bytes_per_frame, expected * r.bytes_per_frame));
                CHECK(info.frames == expected && info.first_frame == slot->info.first_frame + slot->offset);
                CHECK(info.device_position == slot->info.device_position + slot->offset);
                CHECK(info.clock_epoch == slot->info.clock_epoch && info.generation == slot->info.generation);
                uint64_t ticks = slot->info.time_remainder + (uint64_t)slot->offset * UINT64_C(1000000000);
                uint64_t delta = ticks / slot->info.frame_time_denominator;
                uint32_t flags = slot->info.flags;
                if (slot->offset) {
                    flags &= ~(HA_BLOCK_GAP | HA_BLOCK_DISCONTINUITY | HA_BLOCK_UNKNOWN_GAP);
                    if (delta > UINT64_MAX - slot->info.host_time_ns) flags &= ~HA_TIME_HOST_VALID;
                }
                CHECK(info.flags == flags);
                if (flags & HA_TIME_HOST_VALID) {
                    CHECK(info.host_time_ns == slot->info.host_time_ns + delta);
                    CHECK(info.time_remainder == ticks % slot->info.frame_time_denominator);
                }
                slot->offset += expected;
                if (slot->offset == slot->info.frames) { head = (head + 1u) % r.slots; --count; }
            } else CHECK(hail_ring_read(&r, actual, frames, &info) == 0);
            for (size_t j = expected * r.bytes_per_frame; j < sizeof(actual); ++j) CHECK(actual[j] == 0xa5);
        }
        CHECK((unsigned)(atomic_load(&r.write_index) - atomic_load(&r.read_index)) == count);
    }
    return 0;
}
