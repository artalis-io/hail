# Goal: qualify Hail native streaming on Windows

Status: [owner-authorized Shure native execution](native-windows-physical-results.md)
now passes the 32-case format/startup matrix, simplex/duplex, lifecycle stress,
600 s sustained duplex and matching-runtime ASan profiles after a capture-clock
fix. Owner-coordinated USB removal/reopen also passes with fresh endpoint IDs
and clock domains. The signed VB-CABLE pair was subsequently exercised, but all
four strict 48 kHz stereo cases [failed the PCM oracle](native-windows-paired-results.md).
The [stage-isolation follow-up](wasapi-path-results.md) then passes a separately
predeclared exact low-grid F32 profile in primed/unprimed short and ASan trials.
S16 changes are also present in native SDK capture, and the high-level change
is observable before cable capture in native render loopback. The complete
goal remains open for the full paired matrix and S16 numerical qualification.
A later conditioned low-grid unprimed trial failed on fractional samples at
native flush; the ten-minute attempt failed its final numerical assertion.
Passing short/ASan samples therefore do not certify complete paired transitions.
The subsequent [marker-stream matrices](wasapi-marker-results.md) locate the
ramp in SDK capture data before Hail copying, while the actual submitted render
buffer is exactly on-grid. This resolves the transport-copy question; it does
not make the Windows/cable path transparent or turn either failed matrix into
a passing qualification.
Physical timing is uncalibrated; no physical
latency or audible-tail bound is claimed.

## Objective

Demonstrate and fix Hail's shared-mode WASAPI capture, playback and full-duplex
operation on real Windows audio endpoints. Implement and execute the paired
virtual-device qualification design, then leave repeatable native tests,
measurements and a truthful support matrix.

## Starting evidence

- The shared-mode adapter is implemented in [backend_wasapi.c](../src/backend_wasapi.c).
  Its context worker owns enumeration/notifications; its stream worker owns COM
  audio clients and event-driven servicing.
- Windows static/shared builds, portable tests and installed consumers passed
  [CI run 43919570772](https://github.com/artalis-io/hail/actions/runs/43919570772).
  The hosted runner exposed no audio endpoints. Native audio transfer was not
  exercised, so this is not proof that capture/playback works.
- The [paired-endpoint loopback design](wasapi-loopback-design.md) is complete.
  It requires an independently provisioned signed virtual render/capture pair.
  Its executable, fixture validator and provisioned-runner workflow are now
  implemented; [setup and limitations](wasapi-qualification.md) distinguish them
  from actual Windows loopback execution, which remains outstanding.
- Built-in WASAPI render-loopback is not currently exposed by Hail and does not
  create an endpoint on an empty host. A native render-monitor helper alone
  would not qualify Hail's capture client or duplex path.

## Completion criteria

1. Real native execution demonstrates physical capture, playback and simultaneous
   duplex on selected Windows endpoints, with meaningful PCM/timing observations.
   A controlled paired virtual fixture independently verifies known nonzero PCM
   through both Hail legs.
2. Implement the planned C loopback executable, fixture/endpoint manifest
   validation and a runnable native qualification workflow or local harness.
   Never substitute Hail's virtual backend or an empty enumeration for WASAPI I/O.
3. Test primed/unprimed startup, native flush with uninterrupted capture, stale
   generations, stop/restart, repeated destruction, partial initialization,
   endpoint invalidation/removal and explicit recovery/reopen.
4. Test S16/F32, mono/stereo and common supported rates, including 16/24/44.1/48 kHz.
   Report application/engine formats and conversion separately. Shared mode does
   not prove physical end-to-end format identity. Unsupported cases fail honestly.
5. Run a documented sustained duplex trial of at least ten minutes. Separate
   startup/transitions from sustained xruns and verify deliberate capture
   overflow/playback starvation through counters, silence and discontinuity data.
6. Demonstrate bounded Hail memory, no steady-state Hail allocations and safe
   COM/callback/worker retirement. Run supported memory diagnostics and relevant
   portable/static/shared/consumer/export checks without suppressing findings.
7. Measure software delivery, control/flush timing, scheduling behavior, CPU and
   memory on recorded configurations. Claim physical latency or audible tails
   only with appropriate measured fixtures. Preserve raw evidence and failures.
8. Commit fixes, tests, setup/reproduction instructions and updated platform
   claims. Do not declare production readiness while required native cases are
   untested or unexplained correctness failures remain.

## Prompt to use on the native Windows computer

```text
Work in the Hail repository on this native Windows computer. Achieve the objective
and completion criteria in docs/native-windows-goal.md. Execute and verify the
work rather than stopping at a design or successful compilation.

First inspect the branch, working tree, applicable AGENTS.md instructions,
Windows build, native C11 toolchain, audio services and available endpoints.
Preserve unrelated changes. Read docs/wasapi-loopback-design.md, docs/api.md,
docs/architecture.md, docs/backend-authoring.md, docs/native-backends.md,
docs/build.md and docs/verification.md. Inspect include/hail.h,
src/backend_wasapi.c, src/native.c, src/core.c and src/queue.c.

Reproduce shared WASAPI playback, capture and duplex on explicitly selected real
endpoints. Use normal Windows microphone permissions. Do not persist or upload
microphone audio, change defaults unnecessarily, or select unknown devices by
enumeration index. Confirm any missing device/level choice with the owner.

Implement the paired-endpoint qualification design as test code outside the
portable core. Provision a signed virtual cable only through the owner's approved
installation/image procedure, recording origin, version, package hash, signature,
license/automation terms and reboot requirements. Do not disable driver-signing,
Secure Boot or privacy controls. Do not assume a hosted job can install/reboot a
fixture successfully. If provisioning is unavailable, report the prerequisite
and continue real-endpoint/backend work; do not report loopback as passing.

Validate exact fixture endpoint identities and directions from a manifest.
Implement the C test behind HA_BUILD_NATIVE_TESTS/HA_BACKEND_WASAPI with Windows
clock/allocation hooks, bounded queues and independent PCM/generation oracles.
Test both Hail capture and Hail playback through the pair. Keep any built-in
render-loopback helper explicitly render-only; it must not stand in for duplex.
Use documented transparent or conversion profiles and predeclared numerical
oracles. All-zero capture, weakened tolerances and retry-until-pass are invalid.

Investigate COM initialization/interface ownership, event delivery, native buffer
bounds, capture packet flags, silence, partial startup rollback, pending command
supersession, Stop/Reset/Start and invalidation during retirement. COM service
operations must stay on their owning worker. Close must retain storage until
native and application users are quiescent, even after device loss. A timeout
cannot authorize freeing reachable memory.

Verify playback interruption leaves capture active and stale writes cannot
resurrect old work. Distinguish Hail queue cutoff, engine reset acknowledgement
and samples already held downstream. Preserve independent clock domains and
native timestamp validity; render estimates are not measured DAC timestamps.
Test explicit loss/reopen and fresh runtime incarnations after fixture changes.

Keep ha_/HA_ naming, opaque handles, explicit ownership/conversion/errors, bounded
queues and zero steady-state Hail allocations. Keep Windows/COM types private;
add no mandatory virtual-driver, DSP, codec, speech, networking, inference or
Hull dependency. Prefer a private backend fix to an unnecessary public ABI
change, and explain any unavoidable ABI proposal before implementing it.

Run native static/shared builds and C/C++ installed consumers. Verify any Windows
ASan toolchain/runtime configuration explicitly: HA_SANITIZER in MSVC mode cannot
simply reuse the Linux invocation. Use supported Windows diagnostics where
appropriate; do not claim Windows TSan coverage without actual support/results.
Separate strict Release correctness/performance from instrumented/load profiles;
retain every observed gap, failure and permitted-discontinuity rule.

Record source/executable hashes, OS/CPU/toolchain, endpoint/driver/fixture details,
formats, native periods, capacities, trial durations, scheduling/power settings,
raw diagnostics and measured distributions under docs/. Distinguish compile,
context/enumeration, virtual-cable, physical-device and latency evidence. Preserve
historical reports and update the support matrix only to the level actually run.

Commit and push completed fixes, tests and evidence according to repository
policy. Report working native capture/playback/duplex, resolved defects, actual
measurements and concrete remaining blockers without overstating qualification.
```
