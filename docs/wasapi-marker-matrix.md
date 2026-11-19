# Predeclared marker-stream native-flush matrix, 2026-11-18

This diagnostic matrix is declared before native execution. It uses the existing
`transparent-s16-grid-low-v1` marker producer and exact oracle, 48 kHz stereo
F32, five-second nominal phases, native flush A-to-B, stop/restart C and the
existing deliberate faults. It adds observers, not a fractional-sample exception.

Execute exactly one process per cell, in this order:

1. Native tap mode: `none`, `capture`, `both` (capture plus render loopback).
2. Startup: primed, unprimed.
3. Playback queue capacity: 8 slots, 32 slots of 256 frames.
4. Additional requested pre-flush delay: 0 ms, 7 ms.

This produces 24 trials, each independently silence-conditioned through the
approved cable before opening the marker test. There are no automatic retries.
Capacity is not an observed instantaneous queue depth. Requested delay is not
a claim of exact engine-period alignment; actual control elapsed time is logged.
No ten-minute run belongs to this matrix. If cells fail, retain their full logs
and continue the other predeclared cells without relabeling the oracle result.

All three modes collect bounded fractional/asymmetry statistics from Hail's
render reference and capture. `capture` adds an independent native F32 capture
client on the cable; `both` also adds an independent F32 render-loopback client.
All added SDK clients and data belong to the application I/O thread's own COM
apartment. They never access Hail's worker-owned COM interfaces. Added clients
can change engine scheduling/session behavior, so a tap-assisted pass does not
override an untapped failure.

Each stage stores at most eight synthetic fractional examples per phase, with
observation time, SDK host correlation, sample position, flags and actual channel
values in S16 units. No microphone stream is selected and no bulk PCM recording
is created. Two static rings retain 65,536 stereo F32 frames per leg solely for
native/Hail comparison on the I/O owner. Their bytes are reported outside the
Hail allocation ledger; they are bounded diagnostic storage, not a change in
Hail's memory budget or native allocation contract.

Frame pairing requires the same native device position and host correlations
within 100 ns (the SDK correlation granularity). This tolerance applies only to
clock alignment, never to sample values. Paired samples compare exactly, with
zero numerical tolerance. Timestamp-domain mismatches are counted separately;
an absence of aligned pairs cannot prove either sample equality or inequality.
The marker oracle still checks every Hail capture sample independently.

Interpretation: fractional samples at render reference would implicate the path
before Windows receives the selected PCM. Fractional native capture agreeing
exactly with aligned Hail capture establishes that those samples already exist
at the SDK capture boundary. Fractional render loopback locates an alteration
before the cable capture leg. If instrumentation prevents a failure, or no
correlated samples cover it, retain that uncertainty rather than assuming a
root cause. SDK/render-reference observations do not calibrate physical latency.

## Predeclared follow-up without extra native clients

The 24-case matrix completed with one failure (`none-primed-32-7`), 478
fractional capture frames during flush and zero fractional render-reference
frames. All sixteen extra-client cases passed. Their alignment comparisons
were exact, but they did not observe the failing event; those passes cannot
locate its origin or prove that opening the clients prevents it.

Before further native execution, declare a separate 16-case SDK-boundary
matrix. Build an isolated static Release tree with
`HA_WASAPI_DIAGNOSTICS=ON`. This off-by-default, source-private mode observes
the existing capture `GetBuffer` lease before Hail copying, and the existing
render buffer after Hail fills it but before `ReleaseBuffer`. It opens no
additional clients and uses no heap calls, logging or callbacks on the worker.
Bounded records are owned by that worker and read only after native retirement.
This diagnostic build remains separately labeled; no instrumented pass replaces
the failed ordinary Release result.

Run `-SdkBoundaryMatrix` with modes `none` only, startup primed/unprimed,
queue capacities 8/32 and additional requested delays 0/3/7/11 ms, in that
nesting order. Each of the 16 cases gets its own exact-silence preflight and
one five-second marker trial. Preserve every result, without retries or a
ten-minute run. The exact sample oracle and counter requirements remain intact;
diagnostic snapshots may retire the stream before reporting a final numerical
failure so leased data is never read across owners.
