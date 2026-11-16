#ifndef HA_HAIL_H
#define HA_HAIL_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define HA_CALL __cdecl
#if defined(HA_STATIC)
#define HA_API
#elif defined(HA_BUILDING_LIBRARY)
#define HA_API __declspec(dllexport)
#else
#define HA_API __declspec(dllimport)
#endif
#else
#define HA_CALL
#if defined(__GNUC__)
#define HA_API __attribute__((visibility("default")))
#else
#define HA_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Pre-1.0 API: not yet frozen. No public record may be packed.
 * All input records must be initialized with HA_INIT or a specialized initializer.
 * Outputs require size/version headers; unknown versions and short records fail.
 * Larger version-1 records are accepted; unknown tails are never read/written.
 * Unless specified otherwise, pointers are borrowed for the duration of a call.
 * API reference and threading contract: docs/api.md. */
#define HA_VERSION_MAJOR 0u
#define HA_VERSION_MINOR 1u
#define HA_VERSION_PATCH 0u
#define HA_STRUCT_VERSION 1u
#ifdef __cplusplus
#define HA_INIT(type) []() { type ha_value = {}; \
    ha_value.struct_size = (uint32_t)sizeof(type); \
    ha_value.struct_version = HA_STRUCT_VERSION; return ha_value; }()
#else
#define HA_INIT(type) { .struct_size = (uint32_t)sizeof(type), \
                        .struct_version = HA_STRUCT_VERSION }
#endif

typedef struct ha_context ha_context;
typedef struct ha_stream ha_stream;
typedef int32_t ha_result;
#define HA_OK                   ((ha_result)0)
#define HA_PENDING              ((ha_result)1)
#define HA_WOULD_BLOCK          ((ha_result)2)
#define HA_BUSY                 ((ha_result)3)
#define HA_TOO_SMALL            ((ha_result)-1)
#define HA_INVALID_ARGUMENT     ((ha_result)-2)
#define HA_INVALID_STATE        ((ha_result)-3)
#define HA_UNSUPPORTED          ((ha_result)-4)
#define HA_FORMAT_UNSUPPORTED   ((ha_result)-5)
#define HA_PERMISSION_DENIED    ((ha_result)-6)
#define HA_DEVICE_UNAVAILABLE   ((ha_result)-7)
#define HA_DEVICE_LOST          ((ha_result)-8)
#define HA_BACKEND_INIT_FAILED  ((ha_result)-9)
#define HA_RESOURCE_EXHAUSTED   ((ha_result)-10)
#define HA_STREAM_FAILED        ((ha_result)-11)
#define HA_STALE_GENERATION     ((ha_result)-12)
#define HA_CANCELLED            ((ha_result)-13)
#define HA_OVERFLOW             ((ha_result)-14)

#define HA_BACKEND_AUTO 0u
#define HA_BACKEND_VIRTUAL 1u
#define HA_BACKEND_COREAUDIO 2u
#define HA_BACKEND_WASAPI 3u
#define HA_BACKEND_PIPEWIRE 4u
#define HA_BACKEND_ALSA 5u
#define HA_CAPTURE 1u
#define HA_PLAYBACK 2u
#define HA_PCM_S16 1u
#define HA_PCM_F32 2u
#define HA_LAYOUT_MONO 1u
#define HA_LAYOUT_STEREO 2u
#define HA_INTERLEAVED 1u
#define HA_PLANAR 2u
#define HA_EXACT_END_TO_END 0u
#define HA_EXACT_APPLICATION 1u
#define HA_SELECT_DEFAULT 0u
#define HA_SELECT_RUNTIME_ID 1u
#define HA_SELECT_PERSISTENT_KEY 2u
#define HA_IDENTITY_OS_SCOPED 1u
#define HA_DEVICE_KEY_CAPACITY 512u
#define HA_ROLE_DEFAULT 0u
#define HA_ROLE_COMMUNICATIONS 1u
#define HA_STOPPED 0u
#define HA_STARTING 1u
#define HA_RUNNING 2u
#define HA_STOPPING 3u
#define HA_FAILED 4u
#define HA_CLOSING 5u
#define HA_FLUSH_NATIVE_REQUIRED 1u
#define HA_FEATURE_VIRTUAL 1u
#define HA_FEATURE_RENDER_REFERENCE 2u
#define HA_FEATURE_NATIVE_FLUSH 4u
#define HA_FEATURE_COREAUDIO 8u
#define HA_FEATURE_WASAPI 16u
#define HA_FEATURE_PIPEWIRE 32u
#define HA_FEATURE_ALSA 64u
#define HA_CLOCK_INDEPENDENT 1u
#define HA_CLOCK_SHARED 2u
#define HA_CLOCK_UNKNOWN 0u
#define HA_TIME_HOST_VALID 1u
#define HA_TIME_DEVICE_VALID 2u
#define HA_TIME_ESTIMATED 64u
#define HA_CONVERSION_NONE 0u
#define HA_CONVERSION_NATIVE 1u
#define HA_CONVERSION_SERVER 2u
#define HA_CONVERSION_UNKNOWN 3u
#define HA_BLOCK_GAP 4u
#define HA_BLOCK_DISCONTINUITY 8u
#define HA_BLOCK_SILENCE 16u
#define HA_BLOCK_UNKNOWN_GAP 32u
#define HA_EVENT_STATE 1u
#define HA_EVENT_FLUSH 2u
#define HA_EVENT_DEVICE 3u
#define HA_EVENT_DEFAULT 4u
#define HA_EVENT_XRUN 5u
#define HA_EVENT_CLOCK 6u
#define HA_EVENT_RESCAN_REQUIRED 1u
#define HA_COUNTER_SATURATED 2u
#define HA_MAX_DEVICES 64u
#define HA_MAX_STREAMS 16u
#define HA_MAX_EVENTS 1024u
#define HA_MAX_QUANTUM 8192u
#define HA_MAX_QUEUE_SLOTS 256u

typedef struct ha_format {
    uint32_t struct_size, struct_version;
    uint32_t sample_rate, channels, sample_format, channel_layout, storage;
    uint32_t reserved;
} ha_format;
#define HA_FORMAT_INIT(rate, channels_, pcm) \
    { (uint32_t)sizeof(ha_format), HA_STRUCT_VERSION, (rate), (channels_), (pcm), \
      ((channels_) == 1u ? HA_LAYOUT_MONO : HA_LAYOUT_STEREO), HA_INTERLEAVED, 0u }

typedef struct ha_device_selector {
    uint32_t struct_size, struct_version;
    uint32_t kind, role;
    uint64_t runtime_id;
    uint32_t key_bytes, reserved;
    char key[HA_DEVICE_KEY_CAPACITY];
} ha_device_selector;

/* The copied callbacks/user must outlive context destruction. Serialized control
 * calls only. Alloc must honor power-of-two alignment. Free receives original
 * byte count/alignment. NULL callbacks select hosted allocation, if built. */
typedef void *(HA_CALL *ha_allocate_fn)(void *user, size_t bytes, size_t alignment);
typedef void (HA_CALL *ha_deallocate_fn)(void *user, void *ptr,
                                        size_t bytes, size_t alignment);
typedef struct ha_allocator {
    uint32_t struct_size, struct_version;
    ha_allocate_fn allocate;
    ha_deallocate_fn deallocate;
    void *user;
} ha_allocator;

typedef struct ha_diagnostic {
    uint32_t struct_size, struct_version;
    uint32_t backend, operation;
    int64_t native_code;
    ha_result result;
    uint32_t reserved;
} ha_diagnostic;

typedef struct ha_context_config {
    uint32_t struct_size, struct_version;
    uint32_t backend, max_devices, max_streams, event_capacity;
    uint64_t memory_limit;
    ha_allocator allocator;
    ha_diagnostic *diagnostic; /* Optional writable output, not retained. */
} ha_context_config;

typedef struct ha_device_info {
    uint32_t struct_size, struct_version;
    uint64_t runtime_id;
    uint32_t direction, backend, is_default, capabilities;
    uint32_t name_bytes, reserved;
    char name[64]; /* Copied UTF-8, NUL-terminated, no persistent ID promise. */
    uint32_t persistent_key_bytes, identity_flags;
    char persistent_key[HA_DEVICE_KEY_CAPACITY];
} ha_device_info;

typedef struct ha_stream_config {
    uint32_t struct_size, struct_version;
    uint32_t directions, negotiation;
    ha_device_selector capture_device, playback_device;
    ha_format capture_format, playback_format;
    uint32_t quantum_frames, capture_slots, playback_slots, reference_slots;
    ha_diagnostic *diagnostic; /* Optional writable output, not retained. */
} ha_stream_config;

typedef struct ha_memory_plan {
    uint32_t struct_size, struct_version;
    uint64_t bytes;
    uint32_t alignment, allocations;
} ha_memory_plan;

typedef struct ha_context_status {
    uint32_t struct_size, struct_version;
    uint64_t allocated_bytes, memory_limit, lost_events;
    uint32_t streams, flags, backend, features;
} ha_context_status;

typedef struct ha_probe_info {
    uint32_t struct_size, struct_version;
    ha_format application_format, device_format;
    uint32_t device_format_valid, conversion, capabilities, reserved;
    ha_format backend_format;
    uint32_t backend_format_valid, reserved2;
} ha_probe_info;

typedef struct ha_stream_info {
    uint32_t struct_size, struct_version;
    ha_format capture_format, playback_format;
    uint64_t capture_device_id, playback_device_id;
    uint64_t capture_clock_domain, playback_clock_domain;
    uint64_t allocated_bytes;
    uint32_t directions, quantum_frames, capture_slots, playback_slots;
    uint32_t reference_slots, features, clock_relationship, conversion;
    ha_probe_info capture_negotiated, playback_negotiated;
} ha_stream_info;

typedef struct ha_block_info {
    uint32_t struct_size, struct_version;
    uint64_t first_frame, clock_domain, clock_epoch, generation;
    uint64_t device_position, host_time_ns, uncertainty_ns, lost_frames;
    /* Nanoseconds per application frame, as a rational. Remainder belongs to
     * the first-frame correlation and permits exact partial-read interpolation.
     * Valid only with HA_TIME_HOST_VALID and nonzero denominator. */
    uint64_t frame_time_numerator, frame_time_denominator, time_remainder;
    uint32_t frames, flags, sample_rate, reserved;
} ha_block_info;

typedef struct ha_stream_status {
    uint32_t struct_size, struct_version;
    uint32_t state;
    ha_result terminal_result;
    uint64_t playback_generation, flush_completed_generation;
    ha_result flush_result;
    uint32_t flags;
    uint64_t capture_lost_frames, playback_underrun_frames, reference_lost_frames;
    uint64_t capture_epoch, playback_epoch;
    ha_diagnostic diagnostic;
} ha_stream_status;

typedef struct ha_event {
    uint32_t struct_size, struct_version;
    uint32_t type, state;
    uint64_t stream_id, device_id, generation;
    ha_result result;
    uint32_t flags;
} ha_event;

typedef struct ha_version_info {
    uint32_t struct_size, struct_version;
    uint32_t major, minor, patch, abi_major, abi_minor, features;
} ha_version_info;

HA_API ha_result HA_CALL ha_get_version(ha_version_info *out);
/* Immutable static string, valid until library unload; never free or mutate. */
HA_API const char *HA_CALL ha_result_string(ha_result result);
HA_API ha_result HA_CALL ha_context_plan(const ha_context_config *, ha_memory_plan *out);
HA_API ha_result HA_CALL ha_context_create(const ha_context_config *, ha_context **out);
HA_API ha_result HA_CALL ha_context_refresh(ha_context *);
HA_API ha_result HA_CALL ha_context_enumerate(ha_context *, ha_device_info *,
                                             uint32_t capacity, uint32_t *count);
/* One serialized control owner. Poll progresses at most max_streams operations.
 * Output event records must each have initialized size/version headers. */
HA_API ha_result HA_CALL ha_context_poll(ha_context *, ha_event *,
                                        uint32_t capacity, uint32_t *count);
HA_API ha_result HA_CALL ha_context_get_status(ha_context *, ha_context_status *out);
/* With no live streams, unregisters native listeners and frees the context.
 * PENDING retains the handle; retry destruction, with all other users quiescent.
 * No new operations may start after pending context destruction. */
HA_API ha_result HA_CALL ha_context_destroy(ha_context **);
HA_API ha_result HA_CALL ha_device_probe(ha_context *, const ha_device_selector *,
                                        uint32_t direction, const ha_format *,
                                        ha_probe_info *out);
HA_API ha_result HA_CALL ha_stream_plan(ha_context *, const ha_stream_config *,
                                       ha_memory_plan *out);
HA_API ha_result HA_CALL ha_stream_open(ha_context *, const ha_stream_config *,
                                       ha_stream **out);
HA_API ha_result HA_CALL ha_stream_get_info(ha_stream *, ha_stream_info *out);
HA_API ha_result HA_CALL ha_stream_get_status(ha_stream *, ha_stream_status *out);
HA_API ha_result HA_CALL ha_stream_start(ha_stream *);
HA_API ha_result HA_CALL ha_stream_stop(ha_stream *);
/* One reader, one writer, and one reference reader may run concurrently.
 * Read copies up to one slot, retaining any unread suffix. No pointer retention.
 * Write accepts a bounded prefix; zero accepted on errors. Zero frames allows
 * NULL PCM, but output pointers/record headers remain required. */
HA_API ha_result HA_CALL ha_stream_read(ha_stream *, void *, uint32_t capacity_frames,
                                       uint32_t *frames, ha_block_info *out);
HA_API ha_result HA_CALL ha_stream_write(ha_stream *, const void *, uint32_t frames,
                                        uint64_t generation, uint32_t *accepted);
HA_API ha_result HA_CALL ha_stream_read_reference(ha_stream *, void *,
                                                 uint32_t capacity_frames,
                                                 uint32_t *frames, ha_block_info *out);
/* Pending flush blocks writes until poll observes backend acknowledgement.
 * BUSY means no flush accepted. Always pass the generation of the work, never
 * automatically relabel stale work with the latest generation. */
HA_API ha_result HA_CALL ha_stream_flush_playback(ha_stream *, uint32_t flags,
                                                 uint64_t *new_generation);
HA_API ha_result HA_CALL ha_stream_close_begin(ha_stream *);
/* Caller must quiesce ALL application users/aliases before final destruction.
 * PENDING retains storage. Success NULLs caller slot. NULL slot is a no-op. */
HA_API ha_result HA_CALL ha_stream_close_finish(ha_stream **);

#ifdef __cplusplus
}
#endif
#endif
