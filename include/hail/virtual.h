#ifndef HA_VIRTUAL_H
#define HA_VIRTUAL_H
#include <hail.h>
#ifdef __cplusplus
extern "C" {
#endif

/* All virtual operations share the context's serialized control role. They may
 * overlap the dedicated application reader/writer roles, never each other or
 * context_poll. No thread or clock is created. Native/device clocks are modeled,
 * not measured. Each step selects at most one quantum per leg. */
#define HA_VIRTUAL_FAIL_START 1u
#define HA_VIRTUAL_FAIL_STOP 2u
#define HA_VIRTUAL_FAIL_FLUSH 4u
#define HA_VIRTUAL_HOLD_RETIREMENT 8u
#define HA_VIRTUAL_PERMISSION_DENIED 16u
#define HA_VIRTUAL_DISCONTINUITY_CAPTURE 1u
#define HA_VIRTUAL_DISCONTINUITY_PLAYBACK 2u
#define HA_VIRTUAL_UNKNOWN_CAPTURE_GAP 4u

typedef struct ha_virtual_config {
    uint32_t struct_size, struct_version;
    uint32_t faults, start_delay_polls, stop_delay_polls, flush_delay_polls;
    int32_t capture_drift_ppm, playback_drift_ppm;
    uint64_t host_time_ns;
} ha_virtual_config;

typedef struct ha_virtual_step_info {
    uint32_t struct_size, struct_version;
    uint32_t capture_frames, playback_frames, flags, reserved;
    uint64_t advance_ns;
    const void *capture_data; /* NULL generates deterministic synthetic PCM. */
} ha_virtual_step_info;

HA_API ha_result HA_CALL ha_virtual_configure(ha_stream *, const ha_virtual_config *);
/* Playback inspection buffer must fit playback_frames; no partial stepping on
 * short capacity or invalid config. playback_info anchors selected PCM, not DAC
 * presentation. Zero playback frames permits NULL playback buffer. */
HA_API ha_result HA_CALL ha_virtual_step(ha_stream *, const ha_virtual_step_info *,
                                        void *playback, uint32_t capacity_frames,
                                        ha_block_info *playback_info);
/* Disconnect fails every stream pinned to that incarnation. Reconnect creates
 * a fresh runtime ID and makes it default. Enumeration is permission-independent.
 * The returned ID is zero after disconnect. A direction is exactly one leg. */
HA_API ha_result HA_CALL ha_virtual_device_set(ha_context *, uint32_t direction,
                                              uint32_t connected, uint32_t permitted,
                                              uint64_t *runtime_id);
/* Add independent endpoints to exercise pinning and default changes without
 * removal. At most context max_devices entries. IDs are never reused. */
HA_API ha_result HA_CALL ha_virtual_device_add(ha_context *, uint32_t direction,
                                              uint32_t permitted, uint64_t *runtime_id);
HA_API ha_result HA_CALL ha_virtual_default_set(ha_context *, uint32_t direction,
                                               uint64_t runtime_id);
#ifdef __cplusplus
}
#endif
#endif
