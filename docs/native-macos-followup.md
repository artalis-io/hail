# Native macOS follow-up qualification

Further native execution on 2026-11-17 extends the [first physical report](native-macos-results.md).
Physical capture/playback and the ten-minute nominal trial remain established.
This follow-up adds a loaded trial, callback/control instrumentation, real HAL
aggregate removal/replacement, default-change tests, and a demonstrated rollback
fix. A later real OS-denial rerun now passes after a separate denial-reporting
fix. It does **not** claim physical USB/Bluetooth unplug qualification. No calibrated physical loopback fixture is present.

## Completed execution

The host, devices, 48 kHz nominal rates, 512-frame native periods, quantum 256,
64 capture slots and 16 playback slots match the first report. Low-power mode
and normal scheduling remain unchanged. Playback uses digital silence in the
loaded trial; capture samples remain in memory and are discarded. Native test
streams stop at teardown. No sound is generated during builds or documentation.

[The ten-minute loaded trial](evidence/native-macos-followup/trace-loaded-duplex.json)
runs four independent Python SHA256 loops over 1 MiB buffers. This is a defined
four-process CPU load, not saturation of every CPU core. Compilation and a
permission-host experiment also ran during the interval. Native rollback and
aggregate tests ran near the end of the process; the profile is not an isolated
CPU-only laboratory benchmark. The captured source/binary hashes identify the
pre-rollback-fix diagnostic executable; normal successful-start behavior is
unchanged by the later rollback fix.

| Observation | Measured result |
| --- | --- |
| Nominal interval | 600.000722 seconds |
| Capture over process including transitions | 28,875,264 frames; zero gap observations; one epoch change at explicit restart |
| Xruns | Zero startup, sustained and transition playback underruns; zero capture queue loss |
| Native flush acknowledgement | 57.495 ms; capture reader advanced 3,072 frames during flush |
| Render stop / reset / restart | 11.046792 / 0.005250 / 46.391834 ms, one successful call each |
| Capture admitted callback bodies | 56,397 calls; 6.519 us mean, 49.084 us maximum |
| Render admitted callback bodies | 56,390 calls; 2.424 us mean, 13.625 us maximum |
| Largest callback entry interval, capture / render | 78.034 / 84.211 ms, including deliberate stop/restart and flush transitions |
| Capture timestamp-to-read age | 10.461 ms mean, 39.551 ms maximum; unknown native timestamp uncertainty |
| Hail memory | 293,984 bytes peak with fixed diagnostic histograms; zero steady-state Hail allocations |
| Process resources before extra reopen cycles | 16,695,296 bytes peak RSS; 5.332953 s user + 12.872461 s system CPU |

`HA_COREAUDIO_DIAGNOSTICS=ON` adds bounded private per-leg 10 us histogram bins
and control-call counters. It defaults **off**, changes no installed/public ABI,
and its bytes are included in stream planning. Each callback producer records
only while holding its existing RT admission gate; control snapshots only after
native retirement. The measured admitted body starts at callback entry and ends
before RT snapshot publication and final callback unwind. It is not a measurement
of the entire system/driver callback or a hard deadline guarantee. Histogram last
buckets overflow at 2560 us; exact maxima are retained. Histogram totals were
checked against call counts. Interval and jitter maxima deliberately retain
transitions; no steady-state-only jitter bound is inferred.

The per-call trace identifies native render restart as the largest observed
component of flush on this run, rather than reset. It does not establish why
Apple's native restart takes that time, or justify replacing stop/reset/start
with an unproven equivalent. No audible tail or calibrated ADC/DAC latency is
inferred from these software measurements.

## Native lifecycle and rollback

[The registry test](../tests/coreaudio_registry.c) creates an owned CoreAudio
aggregate around the explicitly selected built-in output UID, submits silence,
starts an active stream, removes the aggregate through the real HAL API, refreshes
the registry, observes terminal `HA_DEVICE_LOST`, retires the old stream, recreates
the same UID, verifies a new runtime incarnation and explicitly reopens it.
The private fixture passed in static and shared Release builds. This is actual
native endpoint removal/reconnection, **not** a physical cable unplug.

The `--defaults` variant briefly publishes its owned aggregate and changes only
the output default to it. A stream opened before the change remains pinned;
a later default open resolves to the fixture. The original output default is
restored before removal. The fixture is then destroyed. A cleanup handler also
restores the default on ordinary test failure. The Release and LLVM ASan/UBSan
variants pass. Final enumeration confirms the original microphone/speaker
defaults and no surviving owned aggregate. Input defaults, system-sound defaults,
volume, physical nominal rates and third-party devices were not modified.

[The SDK fault regression](../tests/coreaudio_faults.c) uses real AUHAL units and
listeners with source-private test wrappers. It makes the second leg's start
fail and the first rollback stop fail once. Before the fix, Hail incorrectly
cleared the first leg's `started` flag, hiding a possibly live unit from later
stop attempts. The retained [before-fix failure](evidence/native-macos-followup/rollback-before-fix.log)
proves the state defect. The backend now preserves possibly-started state after
a failed stop and retries it during stop/retirement. The failed-start leg is also
stopped conservatively. Original start failure remains terminal; no automatic
stream recovery is added. Post-fix Release and ASan/UBSan trials pass.

The same regression registers a real HAL property listener, injects a first
removal error, verifies pending ownership and retry, and verifies that a callback
retirement count keeps destruction pending after successful removal. Permission
OSStatus mappings are tested separately. These are **injected SDK failures**,
not claims of spontaneous driver failures or actual privacy denial. No timeout
allows freeing live callback/native references.

Static/shared portable suites, installed C/C++ consumers, exact Mach-O exports
and Clang static analysis of the instrumented core/backend are checked again.
The source-private fault executable is static-only because shared Hail correctly
hides private symbols. The registry fixture uses the public API in both linkages.

## Real OS-denial rerun and fix

At the owner's request, the dedicated LaunchServices host was rerun with normal
macOS authorization. The OS reported status 2 (denied), yet AUHAL open/start
succeeded and no permission error arrived during the observation window. The
[before-fix rerun](evidence/native-macos-followup/os-permission-rerun.json) correctly
failed. A successful AUHAL start alone was therefore insufficient to satisfy
Hail's explicit permission-result contract. It does not prove that any microphone
samples were accessible; this denied test saved no samples.

A private Objective-C helper now queries the existing AVFoundation authorization
state. Known denied/restricted capture fails `HA_PERMISSION_DENIED` before native
open, is rechecked before start, and is published to existing capture streams
from the context control poll. No request/grant/reset is performed and no query
runs on an audio callback. Undetermined authorization is still not a grant:
the host must obtain physical microphone authorization, while virtual endpoints
may not need a prompt. Playback-only paths do not use the capture guard.

[The post-fix real rerun](evidence/native-macos-followup/os-permission-rerun-after-fix.json)
passes: OS authorization status 2, successful context creation, `open=permission
denied`, and context destruction. The original failure is retained separately.
An authorized physical duplex regression uses digital silence for playback;
static/shared, ASan/UBSan, export and installed-consumer checks are kept separate.
CoreAudio now additionally compiles one private Objective-C source and links
system AVFoundation/Foundation. The portable ABI and public headers are unchanged.
Native revocation polling is implemented, but a live user revocation trial is
not inferred from this denied-at-open result.

## Independent BlackHole fixture

[Fresh disposable macOS CI run 44113827497](https://github.com/artalis-io/hail/actions/runs/44113827497)
on source `015abcf79a7424cd0200a792e55beb55e490957b` passed eight strict Release
processes (static/shared) and three separate ASan/UBSan processes. Release verified
1,123,840 nonzero frames; instrumented verified 392,192. Release software flush
acknowledgement ranged 36.297–101.413 ms. Raw logs, reported-gap lines and driver
metadata are retained under [blackhole-ci](evidence/native-macos-followup/blackhole-ci/)
and [the parsed summary](evidence/native-macos-followup/blackhole-ci-summary.json).
Historical timing-sensitive failures remain in the original qualification report.
This fresh run is on the prior committed source, not evidence for uncommitted code.

The extended fixture test now accepts `HA_TEST_COREAUDIO_RATE`,
`HA_TEST_COREAUDIO_CHANNELS` and `HA_TEST_COREAUDIO_PCM`. The workflow schedules
all sixteen rate/channel/PCM combinations for each linkage, once each. Default
48 kHz stereo F32 retains the original encoding, sequence range, per-phase
minimum and strict continuity checks. S16 uses exact integer encodings with a
4096-frame modular sequence; mono checks its single channel. No sample/order,
stale-generation or strict Release gap checks are disabled. Each rate changes
only the selected BlackHole device on the disposable runner. The local physical
hardware's other nominal rates remain untested; unsupported current-rate requests
are still recorded explicitly.

## Unresolved cases and retained attempts

No removable physical device or calibrated loopback fixture has been supplied.
USB/Bluetooth transport-specific unplug/reconnect, separate physical-device
clock behavior and audible interruption/physical round trip remain outside the
measured scope. Aggregate lifecycle and software timing do not substitute for
those claims. Recording the absence of calibrated fixtures satisfies the goal's
measurement disclosure requirement, not a hardware latency result.

Earlier permission attempts remained unqualified. Direct execution of a separately
signed sandbox app inherited an authorized responsible-process context and did
not observe denial; those attempts failed qualification. LaunchServices gives
the app its own identity: it initially reported undetermined authorization.
An initial version opened AUHAL without first requesting host authorization and
waited inside a native property call, then ultimately started without an explicit
permission error. This is retained as a failed precondition/host-method attempt,
not a passing denial case or proof that captured samples were authorized.

The corrected host uses the existing normal AVFoundation authorization helper
before opening Hail. It waited for an OS decision; no denial decision was
received during the available trial. The waiting test was cancelled and its
logs/stack sample retained. Nothing modifies TCC, grants access, resets privacy
settings or silently substitutes another endpoint. The opt-in
[permission runner](../tools/run_coreaudio_permission.py) makes this reproducible:
launch normally, choose **Don't Allow**, and require an explicit native
`HA_PERMISSION_DENIED` result and quiescent destruction. Those earlier unanswered prompts, missing inputs and authorized hosts remain
unqualified cases. SDK error-mapping tests
remain separate from this real OS test.

Git commit signing also requires unlocking the configured GPG key. The attempted
code commit failed to sign, so the extended BlackHole matrix has not yet been
published/executed on its new source. An older passing CI run cannot stand in
for that final test. Completed changes and evidence remain reviewable locally;
signing preferences are not disabled to bypass this blocker.
