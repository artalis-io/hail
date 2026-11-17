# Goal: qualify Hail native streaming on macOS

Status: partial native qualification. Physical capture/playback/duplex has
been exercised; the full real-time and fixture criteria below are not complete.

Progress on 2026-11-17: [native physical results](native-macos-results.md) now
demonstrate built-in capture/playback/duplex and a ten-minute nominal trial.
The full completion criteria remain open; the report identifies fixture,
hotplug, permission-denial and physical-latency gaps.

[Follow-up execution](native-macos-followup.md) adds loaded duplex, callback and
native flush-call measurements, aggregate removal/replacement/default changes,
a rollback regression/fix and fresh independent BlackHole CI. A real OS denial
rerun exposed and fixed missing explicit denial reporting; the rerun now passes.
The owner authorized unsigned publication as `20791b9`; the extended fixture
format matrix is now running on that revision in CI.

## Objective

Make Hail's CoreAudio capture, playback and full-duplex operation reliable on a
native macOS computer. Fix issues found through actual execution, prove that
playback interruption preserves capture, and leave reproducible tests and honest
platform evidence without expanding the portable ABI unnecessarily.

## Starting evidence

- The AUHAL backend is implemented in [backend_coreaudio.c](../src/backend_coreaudio.c).
- BlackHole 2ch tests pass in static/shared Release builds and a separate
  ASan/UBSan profile on an arm64 macOS CI runner. This is virtual-device evidence,
  not physical microphone/speaker qualification.
- The latest recorded Release run verified 1,124,352 nonzero frames. Observed
  flush acknowledgement was 34.420–100.594 ms; cumulative playback underruns were
  nonzero and include unprimed startup/restart/transitions. Neither figure is a
  physical latency or steady-state-only measurement.
- Earlier instrumented runs exposed clock discontinuities and a delayed forward
  sample-counter jump. Those failures are retained. The instrumented oracle now
  accounts for a reported event preceding a buffered-route gap; strict Release
  continuity checks remain separate. Do not erase this distinction or treat the
  underlying scheduling/driver cause as established.
- See [CoreAudio qualification](coreaudio-qualification.md),
  [raw evidence](evidence/coreaudio-loopback.json), and
  [CI run 43976411781](https://github.com/artalis-io/hail/actions/runs/43976411781).

## Completion criteria

1. Native execution demonstrates physical capture, playback and simultaneous
   duplex on explicitly selected devices, including at least one real device
   pair. A virtual BlackHole pair independently verifies known nonzero PCM.
2. Flush rejects stale writes and preserves capture. Stop/restart, repeated
   open/close, permission denial, device removal/reconnection and default-device
   changes behave according to the documented state/ownership contracts.
3. Primed nominal trials distinguish startup/transitions from sustained xruns.
   Complete a documented sustained duplex trial of at least ten minutes. Any
   errors, lost frames, clock resets or unsupported configurations are reported,
   not hidden or silently substituted.
4. Exercise S16/F32, mono/stereo and 16/24/44.1/48 kHz where the device supports
   them. CoreAudio currently requires the device's nominal rate; unsupported
   requests must fail explicitly. Do not add a core resampler to force a pass.
5. Hail allocations remain bounded and absent from steady-state processing;
   teardown is quiescent and supported sanitizer/static-analysis checks have no
   unresolved Hail memory/arithmetic findings. Keep instrumented timing results
   distinct from Release continuity and performance results.
6. Measure software delivery, control-call duration, flush acknowledgement,
   callback behavior, CPU/memory and xruns on recorded configurations. Measure
   audible interruption/physical round trip only with an appropriate known path;
   record their absence if no such fixture is available. Estimated timestamps
   are not measured hardware latency.
7. Commit fixes, native tests, reproduction instructions, raw results and an
   updated support matrix. Remaining blockers must be explicit; do not declare
   production acceptance solely from virtual-device success.

## Prompt to use on the native macOS computer

```text
Work in the Hail repository on this native macOS computer. Achieve the objective
and completion criteria in docs/native-macos-goal.md. Do the work, not just a plan.

First inspect the current branch, working tree, applicable AGENTS.md instructions,
and the host/toolchain/audio devices. Preserve unrelated changes. Confirm actual
macOS execution rather than treating cross-compilation as a runtime test.

Read docs/api.md, docs/architecture.md, docs/backend-authoring.md,
docs/native-backends.md, docs/coreaudio-qualification.md, docs/build.md and
its referenced evidence. Inspect include/hail.h, src/backend_coreaudio.c,
src/native.c, src/core.c, src/queue.c, tests/coreaudio_loopback.c and
.github/workflows/coreaudio-loopback.yml before changing behavior.

Reproduce the existing BlackHole test on a suitable isolated fixture. Its setup
changes that virtual device's nominal sample rate; it must not inadvertently
reconfigure someone else's active audio setup. Record strict Release and
instrumented results separately, including all failures and allowed gap counts.
Do not weaken sample/order checks, suppress findings, or retry until a pass looks
clean. Diagnose timing, test-oracle assumptions and backend defects separately.

Exercise explicitly selected physical input/output devices and duplex operation
using normal macOS authorization and an appropriate microphone usage description.
Do not bypass privacy controls, change system defaults unnecessarily, upload or
persist microphone audio, or mistake physical silence for a verified known-PCM
loopback. Identify device/level choices with the owner if they are unavailable.

Investigate the recorded slow flush acknowledgements and underruns with primed
playback and separate startup/steady-state counters. Prove capture progress during
render stop/reset/start. Measure where time is spent before optimizing. Inspect
AUHAL buffer leases, callback retirement, listener removal, device-clock resets,
partial-start rollback and close races. Never free live native references after
a timeout or infer an audible-tail guarantee from flush acknowledgement.

Keep ha_/HA_ names and opaque portable handles. Keep platform types, clocks and
threads in the private backend; no DSP, speech, networking, inference or Hull
dependency enters the core. Preserve explicit conversion, clock-domain and
ownership semantics, bounded queues and zero steady-state Hail allocations.
Avoid public ABI changes unless a demonstrated requirement cannot be met by the
current contract; explain any such proposal before implementing it.

Implement focused fixes and meaningful regression tests. Run the relevant native
static/shared, C/C++ consumer, portable, sanitizer and export checks. Record exact
source revision/hashes, OS/CPU/toolchain, device/driver/transport, formats, native
periods, capacities, duration, power/scheduling settings, raw measurements and
unsupported cases under docs/. Preserve the historical evidence.

Commit and push completed changes and evidence according to repository policy.
Report what works on this machine, what failed or was fixed, measured limits and
remaining platform blockers. If a device, permission or measurement fixture is
unavailable, report that concrete gap and continue independent useful work;
never label a skipped or unavailable case as passing qualification.
```
