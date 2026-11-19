# Marker-stream flush isolation, 2026-11-18

The subsequent [standalone native-only follow-up](wasapi-native-only-results.md)
passed 16/16 cases without reproducing the ramp. Its reduced scheduling and
priming model leaves the SDK-boundary observation below unresolved.

**The fractional ramp is already present in the SDK capture buffer before Hail
copies it.** The actual SDK render buffer remains on the exact S16 sample grid
before submission. Eight fractional SDK samples match Hail capture by position,
timestamp and value. This locates the numerical alteration between render
submission and capture delivery, rather than in Hail's capture copy.

The specific Windows engine/driver/cable stage responsible is not identified.
No limiter, fade algorithm or operating-system defect is claimed. The exact
oracle remains unchanged, both failing trials are retained, and paired native
flush transparency remains unqualified. No ten-minute trial was attempted.

## Predeclared matrices

The [24-case plan](wasapi-marker-matrix.md) was committed at `acc8539` before
execution. It used the actual low-grid marker producer and existing exact
oracle, five-second nominal phases, both startup modes, 8/32 playback slots,
0/7 ms requested additional delays and three observer configurations. Each
trial had a separate exact-silence preflight. All 24 trials ran once.

| Observer configuration | Passed | Failed | Observation |
| --- | --- | --- | --- |
| Hail reference/capture only | 7 | 1 | Primed, 32 slots, requested 7 ms: 478 fractional capture frames during flush, zero fractional reference frames. |
| Independent native cable capture added | 8 | 0 | No failing event observed. |
| Independent native capture and render loopback added | 8 | 0 | No failing event observed. |

The failing ordinary Release case had 479 oracle errors in total; that counter
also includes sequence/other checks and is not a count of distinct fractional
frames. Its observed additional control delay was 18.7106 ms, not exactly the
requested 7 ms. Capacity is not a measurement of instantaneous queued frames.

Across the sixteen extra-client trials, 7,012,128 native/Hail capture frame
pairs matched exactly with no mismatches or clock-alignment rejections. Pairing
required equal native positions and host correlations within 100 ns; numerical
comparison had zero tolerance. These passing observations do not explain the
untapped failure or prove that opening extra clients prevents it. Additional
clients may change scheduling or engine behavior, so their passes do not
override the ordinary Release failure.

## Existing SDK buffer leases

A separate [16-case follow-up](wasapi-marker-matrix.md) was committed at
`dbbf2c9` before its native execution. It used an isolated static Release build
with `HA_WASAPI_DIAGNOSTICS=ON`, no additional clients, both startup modes,
8/32 slots and requested delays 0/3/7/11 ms. All sixteen trials ran once:
15 passed and one failed (`none-primed-8-3`). Its observed additional delay
was 17.3753 ms, not exactly 3 ms.

The opt-in observer reads the existing capture packet after `GetBuffer` and
before `hail_native_capture`, and reads the render packet after Hail fills it
and before `ReleaseBuffer`. It modifies no PCM, buffer flags, generation,
queue or SDK command. It performs no logging, allocation or callback on the
worker. Bounded plain records remain worker-owned until native retirement,
then the control owner copies them before backend storage is freed. No SDK
pointer or native lease is retained. SDK-diagnostic artifacts remain separate
from ordinary Release artifacts; an instrumented pass cannot replace them.

The failing SDK-boundary case recorded:

| Observation | Scope | Frames | Fractional frames |
| --- | --- | --- | --- |
| Hail render reference | Flush phase | 50,016 | 0 |
| Actual SDK render packet before release | Whole trial | 504,768 | 0 |
| SDK capture packet before Hail copy | Whole trial | 503,520 | 478 |
| Hail capture | Flush phase | 49,920 | 478 |

All eight stored SDK fractional examples matched Hail capture at the same
position and the exact same recorded host correlation, with identical F32
values. The first was position 295703, host correlation 37601567823766 ns,
left/right values `(3062, -3062.125)` in S16 units. Subsequent examples descend
as `(3052, -3052.125)`, `(3042.125, -3042.25)`, and so on. Both SDK and Hail
observations recorded 468 asymmetric frames and no nonfinite samples. These
bounded examples establish the failing data at the SDK boundary; no claim is
made that a bulk trace of every frame was recorded.

The SDK contracts define the [capture packet and its 100 ns host correlation](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getbuffer)
and the [render packet release boundary](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiorenderclient-releasebuffer).
Hail's control path stops, resets and restarts only the render client for native
flush; the [Reset contract](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-reset)
requires a stopped stream. This investigation demonstrates the altered capture
data, not a violation of those SDK control contracts.

## Validation and limits

Static Debug/Release each pass 12 device-free tests; shared Debug/Release each
pass eight. All four installed C/C++ consumer configurations and normal exact
export checks pass. The diagnostic static archive also retains exactly the
27 public symbols. The SDK-boundary build passes all 12 device-free tests.
Matching LLVM 23 ASan passes those 12 tests plus two explicit short native
profiles: no extra clients and both independent tap clients. Both native ASan
trials pass the sample oracle and balanced two-allocation teardown; this does
not replace either matrix failure or establish Windows TSan coverage.

Device-free negative tests exercise fractional-frame detection, exact native/Hail
pair mismatches, rejection of clock correlations outside 100 ns, and the existing
startup/generation/sequence oracle. Runner-gate integration checks still pass.
Shared or tests-disabled SDK-diagnostic configurations are rejected by CMake;
the SDK-diagnostic CLI rejects physical mode before endpoint selection. The
diagnostic option is off by default, introduces no exported symbol/public ABI,
and changes no normal backend PCM/control behavior.

The native tap comparison uses two bounded 65,536-frame static rings (4 MiB)
outside the Hail allocator ledger. The SDK-boundary records instead live in the
diagnostic backend stream's accounted storage. These are different diagnostic
profiles, not new production memory requirements. There are at most eight
fractional examples per stage/phase in the application observer and eight per
leg in the SDK observer. Only synthetic cable samples are retained; no physical
microphone stream was opened. All six Windows defaults and the muted Shure
level match the session's initial readbacks. No Hail audio process remains live.

The original exact profile, S16 numerical conversion, calibrated physical
latency and the complete paired native-transition matrix remain unqualified.
The remaining distinction is within the Windows/virtual-cable path between
SDK submission and capture. A different fixture or independently specified
transition/conversion oracle would be separate work; no tolerance or fractional
exception has been introduced here.

[Per-case CSV](evidence/wasapi-marker-2026-11-18/matrix.csv),
[hashed result index](evidence/wasapi-marker-2026-11-18/summary.json) and
[raw plans, process records, logs and checks](evidence/wasapi-marker-2026-11-18/raw.zip)
preserve all forty matrix processes, their forty conditioning runs and the
separate memory-diagnostic results. The SDK tree's initial C++ autodetection
failed before execution; explicit `CMAKE_CXX_COMPILER=clang-cl` corrected it,
and its configure log is retained. No case was retried automatically.
