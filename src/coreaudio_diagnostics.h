#ifndef HAIL_COREAUDIO_DIAGNOSTICS_H
#define HAIL_COREAUDIO_DIAGNOSTICS_H
#include <stdint.h>

/* Opt-in qualification build only. Not installed, exported or in the public
 * ABI. Read only after native retirement and application reader quiescence.
 * Callback records are owned by each admitted leg, serialized by its RT gate.
 * Buckets have 10 us width; the last bucket is overflow, not an upper bound. */
#define HAIL_CA_TRACE_BINS 257u
typedef struct hail_ca_callback_trace {
    uint64_t calls, frames, duration_ns, max_duration_ns;
    uint64_t last_entry_ns, last_period_ns, interval_count, interval_ns;
    uint64_t max_interval_ns, max_jitter_ns;
    uint64_t duration_hist[HAIL_CA_TRACE_BINS], jitter_hist[HAIL_CA_TRACE_BINS];
} hail_ca_callback_trace;
typedef struct hail_ca_control_trace {
    uint64_t calls, total_ns, max_ns;
    int32_t last_status;
} hail_ca_control_trace;
typedef struct hail_ca_trace {
    hail_ca_callback_trace leg[2];
    /* Render stop, reset, restart, only during native flush. */
    hail_ca_control_trace flush[3];
} hail_ca_trace;
#endif
