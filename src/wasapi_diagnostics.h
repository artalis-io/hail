#ifndef HAIL_WASAPI_DIAGNOSTICS_H
#define HAIL_WASAPI_DIAGNOSTICS_H
#include <stdint.h>
/* Opt-in static native-test build only; not installed or exported. The stream
 * worker owns these records. Snapshot only after native retirement, before
 * close_finish frees backend_state. No callbacks, allocation or logging. */
#define HAIL_WA_DIAG_EXAMPLES 8u
typedef struct hail_wa_diag_example { uint64_t host_ns,position; float left,right; } hail_wa_diag_example;
typedef struct hail_wa_diag_leg {
    uint64_t frames,fractional,asymmetric,nonfinite;
    uint32_t examples;
    hail_wa_diag_example example[HAIL_WA_DIAG_EXAMPLES];
} hail_wa_diag_leg;
typedef struct hail_wa_diag { hail_wa_diag_leg leg[2]; } hail_wa_diag;
#endif
