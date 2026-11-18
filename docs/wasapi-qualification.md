# Windows native qualification harness

The [marker-stream investigation](wasapi-marker-results.md) now locates the
fractional native-flush ramp in SDK capture data before Hail copying. Both
predeclared matrices retain one failure; exact paired transition qualification
remains open. `tools/run_wasapi_marker_matrix.ps1` reproduces the declared cells
and preserves every result. `HA_WASAPI_DIAGNOSTICS=ON` is an off-by-default,
static/native-test-only buffer observer, not a normal production build.

Implemented on 2026-11-18, with [owner-authorized Shure native execution](native-windows-physical-results.md)
now covering the format/startup matrix, ten-minute duplex, real USB loss/reopen
and ASan. The installed VB-CABLE's [first paired campaign](native-windows-paired-results.md)
failed the exact PCM oracle; paired qualification and calibrated physical timing remain open. See
[local results](native-windows-results.md) for historical evidence and blockers.
The [design](wasapi-loopback-design.md) remains the acceptance contract.

`HA_BUILD_NATIVE_TESTS=ON` and `HA_BACKEND_WASAPI=ON`, with tests enabled, build
`hail_wasapi_loopback`. Ordinary CTest runs only the device-free oracle, packet,
command, notification and retirement tests. It never opens audio streams.

From a Windows developer shell with ClangCL, Ninja, CMake, Python and the SDK:

```powershell
./tools/check_windows_builds.ps1
./build-windows-validation/OFF-Release/hail_wasapi_loopback.exe --list
```

The first command runs static/shared Debug/Release checks, exact PE/COFF symbol
checks and installed C/C++ consumers. Enumeration emits JSON records containing
the exact persistent endpoint keys. Enumeration indices and defaults are never
used for selection by this harness.

## Signed paired fixture

Copy [manifest.example.json](../tests/fixtures/wasapi/manifest.example.json) to
an owner-controlled location and fill it from the approved provisioning records.
The example intentionally fails validation. No script installs a driver, accepts
a license, reboots, changes default endpoints or modifies privacy/security policy.

The validator verifies approval/reservation declarations, package and driver
SHA-256, a valid pinned driver signer, installed signed PnP driver version/INF,
active exact Hail endpoint identities/directions and their PnP ancestry to the
manifest owner. An unprovable ownership relationship fails; a friendly name or
shared container label is insufficient. Provisioner declarations about effects,
routing, exclusivity and licensing remain assertions, not independent proofs.
Native PCM verification is mandatory after manifest validation.

The numerical profiles are `transparent-s16-grid` and the separately predeclared
[`transparent-s16-grid-low-v1`](wasapi-low-grid-profile.md). The latter uses an
11-bit counter and values below one quarter scale; it is not evidence that the
original near-full-scale profile passed. [Stage isolation](wasapi-path-results.md)
records passing low-grid F32 short/ASan cases and independently observed S16
conversion noise. Both profiles apply exact zero-tolerance checks. Application
and engine rates/channels must match, endpoint/channel gains must be unity and
mute disabled. Samples occupy an exactly representable S16 grid in S16 or F32.
Left values encode marker A/B/C and a 13-bit frame sequence; stereo right is the
negative left. No tolerance is applied. Each marker must deliver more than one
third of a second of verified nonzero frames. Zeros alone cannot pass. The
sequence repeats after 8,192 frames; metadata independently checks logical/raw
positions and epochs. Numerical resampling/conversion profiles are **not yet
implemented** and cannot be qualified by relaxing this oracle.

For the low-grid profile the sequence repeats after 2,048 frames and the left
value is `marker * 2048 + sequence`, divided by 32,768. The example above about
13-bit sequences describes the original profile. The runner selects the correct
signal from the manifest's profile field; keep campaigns in separate output
directories. Stop/reset may discard queued old-generation data: the oracle
records and validates residual old tail separately, then requires strict
continuity for the new generation and rejects old-after-new samples.

```powershell
./tools/run_wasapi_qualification.ps1 `
  -Manifest C:/fixtures/hail/48k-stereo.json `
  -Executable ./build-windows-validation/OFF-Release/hail_wasapi_loopback.exe `
  -OutputDirectory ./build-wasapi-evidence
```

The runner attempts both representations and primed/unprimed startup for every
declared matching fixture, and a 600-second sustained trial using the first
manifest. The full matrix needs matching approved manifests for 16/24/44.1/48 kHz,
mono/stereo. Pass these as a PowerShell string array to `-Manifest`. Missing
profiles produce retained `*.unqualified.txt` records and a failing aggregate
result. A single 48 kHz stereo fixture does not pass the whole format matrix.
No failed run is retried automatically. Before every trial the sibling
`hail_wasapi_path.exe` performs a separate exact F32-silence preflight through
the selected cable to clear downstream content left by prior failed trials.
Supply `-ConditioningExecutable` if it has a different path. A failed F32
primed/unprimed short case blocks the sustained attempt; an S16 failure still
fails the aggregate even when the separate F32 subset passes.

The current [follow-up](wasapi-path-results.md) retains a later conditioned
unprimed failure on fractional flush samples. Earlier passing F32 short/ASan
samples and a zero-loss measured phase therefore do not establish a complete
transition or sustained numerical pass. `tests/wasapi_runner.ps1` tests the
sustained gate with device-free mock processes; it never opens audio devices.

The manually dispatched `WASAPI provisioned paired-endpoint qualification`
workflow requires the `self-hosted, Windows, X64, hail-wasapi-loopback` labels and
a JSON array of provisioned manifest paths. It serializes static/shared
Debug/Release jobs and preserves evidence on failure. It does not provision a
host or claim that hosted Windows runners contain audio devices. Physical
testing and provisioner-controlled loss tests remain separate from this job.

## Lifecycle, allocation and measurement

The application I/O thread owns capture reads, writes and reference reads;
the main thread owns all control/status operations. Capture draining continues
while control requests native flush. A/B verification rejects old content after
new content; writers keep the old generation until acknowledgement. The test
joins the writer before a separate explicit stale-write assertion, stops,
restarts with C, deliberately starves render and overflows capture, and checks
silence/counter observations. Final close waits for worker retirement after
application thread join. Failed deadlines terminate the test process without
freeing reachable native/application storage.

Strict sustained counters are checked separately from startup and deliberate
faults. In strict paired runs, logical capture gaps and epoch changes through
flush also fail. `--instrumented` records marked forward discontinuities;
corrupt samples, unmarked gaps and generation regressions still fail. It must
be reported as a different profile, never as a replacement for strict Release.

Counting aligned allocator hooks verify exact plans, allocation sizes/alignment,
bounded Hail bytes, no Hail allocations during processing/control, balanced
freeing and repeated NULL-slot destruction. Windows engine/driver memory is
outside this ledger. Native query helpers report mix formats, device periods
and observed buffer sizes on a separate, local COM owner; they never operate on
Hail's worker-owned interfaces. Shared-mode physical format remains unknown.

Logs contain sustained loss/underrun/reference counts, process CPU and working
set, polling/I/O interval histograms (1 ms buckets, overflow bucket at 1,023 ms),
flush acknowledgement timing and capture progress during the flush trial.
First-marker write-to-capture-read observations and last-A capture-read relative
to flush request describe the buffered software path. There are only three
marker observations per trial; these are not a per-frame delivery distribution,
physical latency or an audible-tail bound. Raw clocks and physical ADC/DAC
latency still require an appropriate independent fixture/measurement campaign.

For actual device removal/reopen, explicitly supply the provisioner's approved
scripts. They must accept `-PnpInstanceId`, wait for their operation to complete,
throw on failure, and affect only that exact manifest device:

```powershell
./tools/run_wasapi_loss.ps1 -Manifest C:/fixtures/hail/48k-stereo.json `
  -Executable ./build-windows-validation/OFF-Release/hail_wasapi_loopback.exe `
  -OutputDirectory ./build-wasapi-loss `
  -DisableFixtureScript C:/fixtures/hail/disable.ps1 `
  -EnableFixtureScript C:/fixtures/hail/enable.ps1
```

Named events coordinate removal during active I/O, terminal DEVICE_LOST,
application join/native close, restoration and explicit reopen in the same
context. Both endpoint incarnations and clock domains must change. The wrapper
attempts restoration even on failure. This mode has not run without a fixture.

## Physical endpoints and diagnostics

Select approved exact keys from `--list`, then run `--physical --capture KEY
--playback KEY --seconds 600`. Omit one key for simplex. Other options are
`--rate 48000 --channels 2 --pcm f32` (or `s16`) and `--unprimed`. Physical mode
uses a quiet triangle, peak 504/32,768 (below -36 dBFS), and saves only measurements.
It checks finite/nonzero microphone PCM, timing, reference silence, lifecycle and
counters; acoustic PCM does not use the cable's exact-value oracle. It does not
certify that a human heard playback. Confirm endpoint and output level before
running. Stop devices at the end of each trial; no PCM recording is retained.

`tools/run_wasapi_physical.ps1` automates this execution with separate raw logs,
per-second process observations and hashes. Use `-Mode matrix -Seconds 1` for all
32 duplex format/startup cases, `capture`/`playback` for simplex, `lifecycle` for
30 transition/destruction cycles, or `duplex -Seconds 600` for sustained operation.
`-TestCaptureGain 0.5` explicitly opts into temporary capture gain/unmute on the
selected capture endpoint. `hail_wasapi_endpoint_level` reads/saves the original
settings and the runner restores them in `finally`, including failed trials.
This helper has its own COM owner, cannot change playback gain, opens no audio
client and is outside the library. Ordinary runs do not alter levels.

`tools/run_wasapi_physical_hotplug.ps1` prepares the same-context loss/reopen
trial with human USB unplug/reconnect instead of administrative fixture scripts.
Run only after coordinating the ready signal with the owner; do not unplug during
another audio trial. The owner-coordinated Shure hotplug trial has now passed;
its first no-removal timeout is retained separately in the physical report.

The measured-phase capture timing histogram reports nonnegative SDK host
correlation ages in 100 microsecond buckets and counts future correlations
separately. Use the explicitly conditional quantiles from the summary tool.
Unknown uncertainty and future values prevent physical latency interpretation;
do not silently turn them into zero or omit them from a timing report.

ClangCL x64 with the DLL CRT can now build `-DHA_SANITIZER=address` in an isolated
tree. This is ASan only; POSIX address mode still means ASan+UBSan. CMake locates
the matching compiler resource runtime/import library/thunk; put its
`lib/windows` directory on the test process PATH. Follow the
[Clang runtime linking requirements](https://clang.llvm.org/docs/UsersManual.html#finding-clang-runtime-libraries).
The local ClangCL 19.1.5 runtime fails during interceptor initialization on
Windows build 26300; that failure is preserved. Matching LLVM 23.1.6 now passes
9/9 device-free tests and native F32/S16 duplex on this host. Configure with an
explicit ClangCL 23 path in `CMAKE_C_COMPILER` and `CMAKE_CXX_COMPILER`, select
`HA_SANITIZER=address`, build and add that compiler's `lib/windows` resource path
to the test process PATH. `tools/probes/windows_asan_runtime.c` provides a clean
probe and an intentional `--overflow` negative control; only the negative probe
must fail with ASan. Keep the earlier failure and record compiler/runtime hashes;
do not substitute unmatched runtimes. No Windows TSan/UBSan/LSan result is implied.
Application Verifier is installed locally but requires elevated per-image
configuration; it was not enabled or counted as executed diagnostics.
