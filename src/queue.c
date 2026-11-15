#include "hail_internal.h"

int hail_header(const void *p, uint32_t size)
{
    uint32_t h[2];
    if (!p) return 0;
    memcpy(h, p, sizeof(h));
    return h[0] >= size && h[1] == HA_STRUCT_VERSION;
}

ha_result hail_record(void *out, const void *value, uint32_t size)
{
    if (!hail_header(out, size)) return HA_INVALID_ARGUMENT;
    /* The output's size/version header belongs to the caller. */
    memcpy((unsigned char *)out + 8, (const unsigned char *)value + 8, size - 8u);
    return HA_OK;
}

int hail_add(size_t *n, size_t x)
{
    if (x > SIZE_MAX - *n) return 0;
    *n += x;
    return 1;
}

int hail_mul(size_t a, size_t b, size_t *out)
{
    if (a && b > SIZE_MAX / a) return 0;
    *out = a * b;
    return 1;
}

size_t hail_align(size_t x)
{
    if (x > SIZE_MAX - (HAIL_ALIGNMENT - 1u)) return 0;
    return (x + HAIL_ALIGNMENT - 1u) & ~(size_t)(HAIL_ALIGNMENT - 1u);
}

ha_result hail_format(const ha_format *f, uint32_t *bpf)
{
    if (!HAIL_HEADER(f) || f->reserved) return HA_INVALID_ARGUMENT;
    if (f->sample_rate < 8000 || f->sample_rate > 192000 ||
        (f->channels != 1 && f->channels != 2) ||
        f->storage != HA_INTERLEAVED ||
        f->channel_layout != (f->channels == 1 ? HA_LAYOUT_MONO : HA_LAYOUT_STEREO))
        return HA_FORMAT_UNSUPPORTED;
    if (f->sample_format != HA_PCM_S16 && f->sample_format != HA_PCM_F32)
        return HA_FORMAT_UNSUPPORTED;
    *bpf = f->channels * (f->sample_format == HA_PCM_S16 ? 2u : 4u);
    return HA_OK;
}

int hail_gate_enter(atomic_uint *gate)
{
    unsigned int expected = HAIL_GATE_IDLE;
    return atomic_compare_exchange_strong_explicit(gate, &expected, HAIL_GATE_ACTIVE,
                                                   memory_order_acquire,
                                                   memory_order_relaxed);
}

void hail_gate_leave(atomic_uint *gate)
{
    atomic_store_explicit(gate, HAIL_GATE_IDLE, memory_order_release);
}

void hail_ring_reset(hail_ring *r)
{
    /* Only after producer/consumer retirement and exclusive role gates. */
    atomic_store_explicit(&r->read_index, 0, memory_order_relaxed);
    atomic_store_explicit(&r->write_index, 0, memory_order_relaxed);
    r->offset = 0;
}

uint32_t hail_ring_push(hail_ring *r, const void *src, uint32_t frames,
                         const ha_block_info *info)
{
    uint32_t done = 0;
    unsigned int w = atomic_load_explicit(&r->write_index, memory_order_relaxed);
    unsigned int rd = atomic_load_explicit(&r->read_index, memory_order_acquire);
    while (done < frames && (unsigned int)(w - rd) < r->slots) {
        uint32_t n = frames - done;
        hail_slot *slot;
        if (n > r->quantum) n = r->quantum;
        slot = (hail_slot *)(void *)(r->storage + (w & (r->slots - 1u)) * r->stride);
        slot->info = *info;
        slot->info.first_frame += done;
        slot->info.device_position += done;
        slot->info.frames = n;
        /* Push is used with <= one quantum on backend paths. Application
         * playback slots carry no valid timestamps until render selection. */
        memcpy((unsigned char *)slot + sizeof(*slot),
               (const unsigned char *)src + (size_t)done * r->bytes_per_frame,
               (size_t)n * r->bytes_per_frame);
        ++w;
        done += n;
        atomic_store_explicit(&r->write_index, w, memory_order_release);
    }
    return done;
}

uint32_t hail_ring_read(hail_ring *r, void *dst, uint32_t capacity,
                         ha_block_info *info)
{
    unsigned int rd = atomic_load_explicit(&r->read_index, memory_order_relaxed);
    unsigned int w = atomic_load_explicit(&r->write_index, memory_order_acquire);
    hail_slot *slot;
    uint32_t n;
    if (!capacity || rd == w) return 0;
    slot = (hail_slot *)(void *)(r->storage + (rd & (r->slots - 1u)) * r->stride);
    n = slot->info.frames - r->offset;
    if (n > capacity) n = capacity;
    *info = slot->info;
    info->first_frame += r->offset;
    info->device_position += r->offset;
    if (r->offset && (info->flags & HA_TIME_HOST_VALID)) {
        uint64_t numerator, offset;
        if (!info->frame_time_denominator ||
            info->frame_time_numerator > (UINT64_MAX - info->time_remainder) / r->offset) {
            info->flags &= ~HA_TIME_HOST_VALID;
        } else {
            numerator = (uint64_t)r->offset * info->frame_time_numerator + info->time_remainder;
            offset = numerator / info->frame_time_denominator;
            if (offset > UINT64_MAX - info->host_time_ns) info->flags &= ~HA_TIME_HOST_VALID;
            else {
                info->host_time_ns += offset;
                info->time_remainder = numerator % info->frame_time_denominator;
            }
        }
    }
    if (r->offset) info->flags &= ~(HA_BLOCK_GAP | HA_BLOCK_DISCONTINUITY | HA_BLOCK_UNKNOWN_GAP);
    info->frames = n;
    memcpy(dst, (unsigned char *)slot + sizeof(*slot) +
           (size_t)r->offset * r->bytes_per_frame, (size_t)n * r->bytes_per_frame);
    r->offset += n;
    if (r->offset == slot->info.frames) {
        r->offset = 0;
        atomic_store_explicit(&r->read_index, rd + 1u, memory_order_release);
    }
    return n;
}

void hail_counter(ha_stream *s, uint64_t *counter, uint64_t increment)
{
    if (increment > UINT64_MAX - *counter) {
        *counter = UINT64_MAX;
        s->status.flags |= HA_COUNTER_SATURATED;
    } else *counter += increment;
}

ha_result hail_capture(ha_stream *s, const void *data, uint32_t frames,
                        const ha_block_info *info)
{
    uint32_t n;
    if (!s->capture.slots || frames > s->capture.quantum || (frames && !data))
        return HA_INVALID_ARGUMENT;
    if (frames > UINT64_MAX - s->capture_position) return HA_OVERFLOW;
    n = hail_ring_push(&s->capture, data, frames, info);
    s->capture_position += frames;
    if (n < frames) {
        hail_counter(s, &s->status.capture_lost_frames, frames - n);
        s->capture_flags |= HA_BLOCK_GAP;
        hail_event(s->context, HA_EVENT_XRUN, s, 0, HA_OVERFLOW);
    } else if (frames) s->capture_flags = 0;
    return HA_OK;
}

ha_result hail_render(ha_stream *s, void *data, uint32_t frames, ha_block_info *info)
{
    uint32_t done = 0;
    ha_block_info ignored = HA_INIT(ha_block_info);
    if (!s->playback.slots || frames > s->playback.quantum || (frames && !data))
        return HA_INVALID_ARGUMENT;
    if (frames > UINT64_MAX - s->playback_position) return HA_OVERFLOW;
    while (done < frames && !s->flush_pending) {
        uint32_t n = hail_ring_read(&s->playback,
            (unsigned char *)data + (size_t)done * s->playback.bytes_per_frame,
            frames - done, &ignored);
        if (!n) break;
        done += n;
    }
    if (done < frames) {
        memset((unsigned char *)data + (size_t)done * s->playback.bytes_per_frame,
               0, (size_t)(frames - done) * s->playback.bytes_per_frame);
        info->flags |= HA_BLOCK_SILENCE;
        if (!s->flush_pending) {
            hail_counter(s, &s->status.playback_underrun_frames, frames - done);
            hail_event(s->context, HA_EVENT_XRUN, s, 0, HA_WOULD_BLOCK);
        }
    }
    if (s->reference.slots) {
        uint32_t n = hail_ring_push(&s->reference, data, frames, info);
        if (n < frames) {
            hail_counter(s, &s->status.reference_lost_frames, frames - n);
            s->playback_flags |= HA_BLOCK_GAP;
        } else if (frames) s->playback_flags = 0;
    }
    s->playback_position += frames;
    return HA_OK;
}
