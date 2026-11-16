#ifndef HAIL_INTERNAL_H
#define HAIL_INTERNAL_H
#include <hail.h>
#include <stdatomic.h>
#include <limits.h>
#include <float.h>
#include <string.h>

_Static_assert(CHAR_BIT == 8 && sizeof(float) == 4 && FLT_RADIX == 2 &&
               FLT_MANT_DIG == 24, "Hail requires IEEE binary32 and 8-bit bytes");
_Static_assert(sizeof(unsigned int) == 4 && ATOMIC_INT_LOCK_FREE == 2,
               "Hail concurrent queues require native lock-free 32-bit atomics");
#define HAIL_ALIGNMENT 16u
#define HAIL_HEADER(p) hail_header((p), (uint32_t)sizeof(*(p)))
#define HAIL_RECORD(p, value) hail_record((p), &(value), (uint32_t)sizeof(value))
#define HAIL_GATE_IDLE 0u
#define HAIL_GATE_ACTIVE 1u
#define HAIL_GATE_CLOSED 2u

typedef struct hail_slot { ha_block_info info; } hail_slot;
typedef struct hail_ring {
    unsigned char *storage;
    uint32_t slots, quantum, bytes_per_frame, stride;
    atomic_uint write_index, read_index;
    uint32_t offset; /* Consumer-owned. */
} hail_ring;
typedef struct hail_device {
    ha_device_info info;
    uint32_t permitted;
    uint64_t native_id;
    uint32_t native_flags;
    char key[1024];
    char native_name[256];
} hail_device;

typedef struct hail_rt_snapshot {
    uint64_t lost, underrun, reference_lost, epoch;
    uint32_t events, flags;
} hail_rt_snapshot;
typedef struct hail_rt_leg {
    atomic_uint gate, write_index, read_index;
    atomic_int error;
    int64_t native_error;
    hail_rt_snapshot current, snapshots[2];
    uint64_t last_host, last_device;
    uint32_t host_valid, device_valid;
} hail_rt_leg;

struct ha_context;
struct ha_stream;
/* Source-private seam, not a binary plugin interface. Control methods run on
 * the context owner. Audio entry points never call these control methods. */
typedef struct hail_backend_ops {
    uint32_t id, features;
    ha_result (*context_init)(struct ha_context *);
    ha_result (*probe)(struct ha_context *, uint64_t, const ha_format *, uint32_t,
                       ha_probe_info *);
    ha_result (*open)(struct ha_stream *);
    ha_result (*start)(struct ha_stream *);
    ha_result (*stop)(struct ha_stream *);
    ha_result (*flush)(struct ha_stream *);
    ha_result (*retire)(struct ha_stream *);
    size_t context_bytes, stream_bytes;
    ha_result (*refresh)(struct ha_context *);
    ha_result (*context_free)(struct ha_context *);
    void (*stream_free)(struct ha_stream *);
    void (*context_poll)(struct ha_context *);
} hail_backend_ops;

struct ha_context {
    ha_allocator allocator;
    const hail_backend_ops *backend;
    uint64_t allocated, limit, next_id, lost_events;
    size_t allocation_bytes;
    uint32_t max_devices, max_streams, event_capacity, event_read, event_count;
    uint32_t stream_count, flags, closing;
    uint64_t default_id[2];
    uint32_t device_count;
    hail_device *devices;
    struct ha_stream **streams;
    ha_event *events;
    void *backend_state;
    atomic_uint registry_dirty;
};

struct ha_stream {
    ha_context *context;
    ha_stream_info info;
    ha_stream_status status; /* Serialized control/backend-driver owner only. */
    atomic_uint state, read_gate, write_gate, reference_gate;
    hail_ring capture, playback, reference;
    uint64_t id, generation, capture_position, playback_position;
    uint64_t capture_epoch, playback_epoch;
    size_t allocation_bytes;
    uint32_t flush_pending, flush_flags, flush_cutoff, retire_ready;
    uint32_t capture_flags, playback_flags;
    uint32_t control_ticks;
    /* Virtual adapter storage is embedded for now, but never exposed in ABI. */
    uint32_t faults, start_delay, stop_delay, flush_delay;
    int32_t capture_drift, playback_drift;
    uint64_t host_ns;
    uint64_t clock_ns[2], clock_remainder[2];
    unsigned char *scratch;
    void *backend_state;
    uint32_t native, negotiation;
    uint64_t render_generation;
    atomic_uint flush_requested, rt_flags;
    hail_rt_leg rt[2];
};

int hail_header(const void *, uint32_t size);
ha_result hail_record(void *, const void *, uint32_t size);
int hail_add(size_t *, size_t);
int hail_mul(size_t, size_t, size_t *);
size_t hail_align(size_t);
ha_result hail_format(const ha_format *, uint32_t *bytes_per_frame);
void hail_event(ha_context *, uint32_t type, ha_stream *, uint64_t device, ha_result);
hail_device *hail_device_find(ha_context *, uint64_t);
void hail_fail(ha_stream *, ha_result);
int hail_gate_enter(atomic_uint *);
void hail_gate_leave(atomic_uint *);
void hail_ring_reset(hail_ring *);
uint32_t hail_ring_push(hail_ring *, const void *, uint32_t, const ha_block_info *);
uint32_t hail_ring_read(hail_ring *, void *, uint32_t, ha_block_info *);
void hail_counter(ha_stream *, uint64_t *, uint64_t);
void hail_signal(ha_stream *, uint32_t, uint32_t, ha_result);
void hail_rt_init(ha_stream *);
int hail_rt_enter(ha_stream *, uint32_t);
void hail_rt_leave(ha_stream *, uint32_t);
int hail_rt_close(ha_stream *, uint32_t);
void hail_rt_open(ha_stream *, uint32_t);
void hail_rt_collect(ha_stream *);
void hail_rt_error(ha_stream *, uint32_t, ha_result, int64_t);
ha_result hail_native_capture(ha_stream *, const void *, uint32_t, uint64_t, uint64_t, uint64_t, uint32_t);
ha_result hail_native_render(ha_stream *, void *, uint32_t, uint64_t, uint64_t, uint64_t, uint32_t);
ha_result hail_native_capture_loss(ha_stream *, uint32_t, uint32_t);
ha_result hail_registry_publish(ha_context *, hail_device *, uint32_t, const char *, const char *);
ha_result hail_device_record(hail_device *, uint32_t, const char *, const char *, uint64_t);
void hail_device_persistent(hail_device *);
void hail_registry_remove(ha_context *, const char *, uint64_t, uint32_t);
void hail_rt_final(ha_stream *);
extern const hail_backend_ops *const hail_backends[];
size_t hail_backend_context_bytes(uint32_t);
const hail_backend_ops *hail_backend_select(uint32_t, uint32_t);
ha_result hail_capture(ha_stream *, const void *, uint32_t, const ha_block_info *);
ha_result hail_render(ha_stream *, void *, uint32_t, ha_block_info *);
ha_result hail_clock_info(ha_stream *, uint32_t, uint32_t, ha_block_info *);
#if HA_ENABLE_VIRTUAL
extern const hail_backend_ops hail_virtual_backend;
#endif
#if !HA_FREESTANDING
void *HA_CALL hail_host_allocate(void *, size_t, size_t);
void HA_CALL hail_host_deallocate(void *, void *, size_t, size_t);
#endif
#endif
