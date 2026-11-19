# Gate E: full-duplex speech and DSP readiness

## Convergence follow-up — 2026-11-19 (base a5b247b)

CI follow-up for `3117b11`: 22/24 checks passed; both macOS static jobs compiled
but aborted in the newly added `coreaudio_format` fixture. Its SDK mock expected
the device stream-format property, whereas the actual adapter first enumerates
device streams and then queries the single stream's physical format. The mock
now implements that exact two-query path, checks object/scope, and additionally
verifies that enumeration failure and zero/multiple streams do not reach the
format query. No production code or existing format assertion is changed.
The correction requires a new macOS CI result; the original failing run remains
[available here](https://github.com/artalis-io/hail/actions/runs/44550304218).

**Verdict remains PARTIAL.** The sections below preserve the original audit and
evidence. This follow-up corrects additional PipeWire descriptor defects and
extends verification without changing public types, exports or transport policy.
Physical alignment, calibrated device latency, audible tails and deadlines remain
qualification requirements, not conclusions from virtual tests.

| Finding | Before follow-up | Correction / source |
| --- | --- | --- |
| E1: valid SPA ring data rejected | Confirmed defect; new actual-callback regression fails on a5b247b | `backend_pipewire.c:stream_process` now takes offsets modulo maxsize, clamps size as SPA specifies, validates complete interleaved frames, and copies a wrapped quantum through planned scratch. Includes byte-unaligned splits. |
| E2: neutral-media capture metadata ignored | Confirmed defect; new actual-callback regression returns nonzero PCM | EMPTY chunks and GAP headers produce zero PCM and HA_BLOCK_SILENCE. Corrupt packets still report loss/epoch transitions; silence is not fabricated loss. SDK storage is never modified. |
| E3: recycled render flags retained | Confirmed defect: actual a5b247b callback fills exact nonzero PCM but leaves EMPTY set | Reset output chunk flags when filling it; before/after callback reproduction and permanent regression check the nonzero SDK payload/reference and cleared descriptor. |
| E4: redirected process exit code lost | Confirmed qualification-runner defect on Windows PowerShell; conditioning exited successfully but the observer recorded null and aborted | Retain the process handle in five WASAPI runners. The actual observer is tested with exit 0, exit 7 and watchdog termination under Windows PowerShell and pwsh. Null is never classified as success. Original qualification gate tests are preserved. |
| WASAPI fractional PCM | Implemented copy path, insufficient platform transparency qualification | `tests/wasapi_buffers.c:fractional_capture_test` verifies exact F32 bytes, including signed zero and fractional S16-unit values seen at the SDK boundary. No rounding, clipping or tolerance adjustment is introduced. |
| Repeated/closing cancellation and all discrete counts | Implemented but incomplete verification | Independent installed `examples/dsp-consumer/main.c` now tests each count 1..32, repeated native-required cancellation with distinct generation PCM and queued obsolete samples, capture continuity, render discontinuities, and close canceling pending flush while retirement is held. |
| Variable blocks with concurrent roles | Implemented but incomplete verification | `tests/concurrency.cpp` varies each leg independently through 1..32 frames while reader/writer/reference/control overlap, including flush and shutdown. BUSY remains valid admission contention, not an acknowledged cancellation. |

PipeWire scratch is one capture quantum, included in `stream_extra_bytes` and
the existing allocator/budget plan. A callback accepts at most HA_MAX_QUANTUM
frames and runs at most ceil(frames/application quantum) iterations. Each wrap
uses at most two bounded copies; neutral data uses one bounded zero fill. No
callback allocation, lock or worker wait is added. Output references remain
selected application PCM before PipeWire/server transformations. This is not a
post-mixer reference. Existing invalid-depth timing estimates remain invalid.

The native descriptor rules follow the primary [SPA chunk contract](https://docs.pipewire.org/structspa__chunk.html)
and [buffer flags](https://docs.pipewire.org/group__spa__buffer.html). They are
also present in the [minimum 0.3.65 SDK's chunk definitions](https://raw.githubusercontent.com/PipeWire/pipewire/0.3.65/spa/include/spa/buffer/buffer.h)
and in the PipeWire 1.0.5 SDK used for the ARM64 regression. Hail still
rejects unsupported planes, strides, incomplete frames and oversized frame counts.

### WASAPI reproduction boundary

The original failures were real shared-mode streaming through signed VB-CABLE
3.5.2.0, not merely SDK mocks. The broad 48 kHz stereo S16/F32 primed/unprimed
trials failed the original strict oracle. Near-full-scale F32 alteration and S16
alteration are retained in [path results](wasapi-path-results.md). The reduced
low-grid profile was independently declared; it did not relax sample tolerance.

The SDK-boundary failure `none-primed-8-3` had 478 fractional captured frames;
submitted SDK-render PCM had none. The identical altered values/positions/QPC
anchors already existed in the SDK capture lease before `hail_native_capture`.
The earlier `none-primed-32-7` failure and negative native-only results remain
valid independent observations. See [marker evidence](wasapi-marker-results.md)
and [native-only evidence](wasapi-native-only-results.md). The locus is between
SDK submission and SDK capture, including the engine, conversion and virtual
driver; the responsible component is **not established**. These results do not
justify modifying Hail's copy arithmetic or promising exact transparent shared
mode on this fixture. Hardware occurrence outside this fixture remains unknown.

`run_wasapi_marker_matrix.ps1 -RegressionCases` predeclares exactly those two
historically failing configurations (primed, no extra clients, 8 slots/3 ms and
32 slots/7 ms), conditions the same approved fixture, records hashes and runs
each once with the unchanged zero-tolerance oracle. A fresh pass cannot erase a
historical intermittent failure or establish a long-run probability bound.

### Calibrated timing trace reduction

The existing platform acquisition runners remain authoritative:
`run_wasapi_physical.ps1`, `run_wasapi_marker_matrix.ps1`,
`run_coreaudio_physical.py`, `run_pipewire_loopback.py`, and `hail_alsa_null` /
the native example for explicit endpoint exercises. Their software observations
are not calibrated physical measurements. The new offline adjunct
`tools/analyze_audio_timing.py` reduces externally collected, matched event
traces from any of those campaigns. It does not acquire microphone data, invent
missing events, provide a DSP algorithm or qualify hardware by itself.

Use a fresh campaign directory. Retain original instrument/SDK logs, conversion
commands, executable/source hashes, OS/device/driver details and an independently
reviewable calibration report. Write `manifest.json` with fields:

```json
{
  "kind": "physical_electrical", "backend": "wasapi",
  "os": "record actual OS/build", "hardware": "record endpoints and interface",
  "driver": "record versions", "sample_rate": 48000, "channels": 2,
  "buffer_frames": 256, "duration_seconds": 600,
  "methodology": "record routing, marker identification and clock mapping",
  "limitations": "record error sources and excluded measurements",
  "calibration": "calibration-report.json"
}
```

This is a template, **not measurement evidence**. Other kinds are `simulated`,
`software_loopback` and `physical_acoustic`. Calibration must be a retained file;
the reducer hashes it. Review its instrument resolution, trigger/marker error,
clock mapping, cable/interface delay and uncertainty budget. Merely supplying a
file does not establish a valid calibration. Predeclare profile-specific
acceptance thresholds and duration before collecting data.

Export CSV with header
`trial,event,time_ns,uncertainty_ns,clock,frame,rate,domain,epoch`.
Use integer ns, a common calibrated monotonic clock for paired events, blank
uncertainty when unavailable, and unique trial/event pairs. Never relabel clocks
to make them match; apply and retain an independently calibrated mapping first.
Frame/rate/domain/epoch are required only for drift anchors. Export each leg's
callbacks in a separate trace; do not merge independent callback schedules.

| Events paired within a matched trial | Interpretation |
| --- | --- |
| capture_anchor → capture_delivery | Public first-frame estimate to application delivery: delivery age, not isolated ADC latency. Unknown estimate uncertainty stays unknown. |
| electrical_input → capture_delivery | Calibrated input-marker-to-delivery latency, including buffering/application scheduling; not isolated ADC latency. |
| submission → electrical_output | Submission of the identified PCM marker to independently observed analog output. |
| submission → electrical_capture | Identified marker returned through the physical loopback and observed at the explicitly documented capture point. Report whether the point is ADC or application delivery; include that path's calibration. |
| reference_anchor → electrical_capture | Signed alignment of the corresponding selected-reference marker and physical return. No sample-perfect cross-device claim. |
| cancel_request → queue_invalidated / flush_ack | Logical invalidation and observed backend acknowledgement are separate distributions. Polling contributes to acknowledgement observation latency. |
| cancel_request → last_presented / last_electrical / last_acoustic | Separate tails. Use only actual presentation receipts or calibrated instruments. A final frame preceding cancellation yields a signed negative tail. Missing receipts remain unavailable. |
| callback_begin → callback_end | Actual instrumented callback execution, never application-pump duration. Optional positive integer callback_deadline_ns and callback_period_ns report observed misses and absolute interval jitter. |

Capture/reference anchor slopes are segmented by leg, domain, epoch, rate and
measurement clock; resets are never fitted across. Endpoint slope includes
timestamp estimation error and is not drift compensation. Store occupancy and
xrun counters in the original platform logs; this tool does not reconstruct
unavailable instantaneous occupancy from submitted-minus-selected frames.

```sh
python tools/analyze_audio_timing.py campaign/manifest.json campaign/trace.csv --output campaign/timing.json
python tests/audio_timing_tool.py
```

The output gives count/median/P95/P99/min/max and summed endpoint uncertainty
bounds, preserving unknowns. Missing metrics are null; cross-clock subtraction,
duplicate events, nonmonotonic same-epoch anchors and uncalibrated physical claims
are errors. Existing output is never overwritten. Tool tests use synthetic data
only. No reported distribution is a physical measurement until its retained
acquisition and calibration evidence have been reviewed.

### Follow-up verification record

Linux ARM64 GCC ASan/UBSan: actual PipeWire descriptor regressions and 13/13
available native tests passed (PipeWire server loopback excluded locally; ALSA
null 2/4/32-channel tests included). Both new capture regressions were first
executed against unchanged a5b247b production source and failed as expected.
The later source changes add an explicit two-quantum wrapped-buffer test for
first-frame positions and estimated timestamp interpolation. CoreAudio's prior
nonfinite-rate correction now has a device-free actual-adapter regression in
`tests/coreaudio_format.c`, registered for static macOS builds without opening a
device. It also checks discrete channel counts and scratch plans. It has not
executed locally; its macOS CI jobs are queued, so it is coverage added, not
verification claimed.

Retained [convergence index](evidence/gate-e-convergence-2026-11-19/summary.json)
and [raw evidence](evidence/gate-e-convergence-2026-11-19/raw.zip) include failures,
source snapshots/hashes, plans, configuration, process observations and logs.
Implementation source begins at e47de77; subsequent commits refine qualification
tools/tests. No local macOS execution or physical timing result is implied.

| Executed follow-up | Result and scope |
| --- | --- |
| Windows x64 clang-cl, static Debug/Release | 16/16 each; exact export/boundary checks and installed consumers 3/3 per configuration |
| Windows x64 clang-cl, shared Debug/Release | 12/12 each; exact DLL exports and installed consumers 3/3 per configuration |
| Windows LLVM 23 ASan, matching runtime | 16/16; independent installed DSP consumer 1/1; later offline-tool regression also passed |
| Linux ARM64 GCC 13.3 static/shared | 14/14 static, 9/9 shared; installed C/C++/DSP consumers 3/3 each; 27 exact public symbols |
| Linux ARM64 GCC ASan/UBSan native | 14/14, including real private PipeWire 1.0.5 software loopback and ALSA 1.2.11 null duplex; installed consumers 3/3, long DSP case ~52 wall seconds |
| Linux ARM64 Clang 18 native | 14/14, same private PipeWire and ALSA null scope |
| Linux ARM64 Clang 18 TSan | Portable 8/8, native 14/14, installed C/C++/DSP consumers 3/3; native framework libraries are not themselves instrumented |
| Linux ARM64 GCC TSan | Seven executables still fail at runtime initialization with unexpected memory mapping; Python test passes. Not a Hail race finding; Clang success does not erase this GCC limitation. No ASLR/security changes made. |
| libFuzzer + ASan/UBSan, Clang 18 ARM64 | config/lifecycle/queue/faults: 30,000 runs each, all passed with retained seed/corpus/logs |
| Linux freestanding boundary | Release conformance passes; ordinary freestanding archive has 27 public symbols, minimal no-virtual archive has 22; portable boundary checks pass |
| Clang static analysis | Windows 10 invocations and Linux 8 invocations (six distinct production files including PipeWire/ALSA), no diagnostics |
| Public concurrent-role CPU-contention smoke | 30/30 Windows runs with two fixed-memory SHA256 workers; 1..32 variable leg blocks, overlapping reads/writes/reference/flush/close. Whole-test median 17.98 ms, maximum 22.62 ms are **not callback duration measurements**. |
| Offline qualification tools | Eight synthetic regression tests passed on Windows and Linux; actual matrix logs reduced by existing WASAPI summarizer |
| Remote CI | Newly pushed jobs remain queued with no runner assigned at evidence capture. No pending macOS, x86 Linux or other CI job is reported as passed. Earlier implementation CI remains historical evidence. |

Two verification-infrastructure failures are retained rather than hidden:
the first new concurrency assertion incorrectly demanded PENDING despite a
concurrent writer legitimately permitting BUSY; it was corrected before final
validation. A strict non-instrumented archive symbol check was also invoked on
an ASan archive and rejected GCC-generated `__odr_asan.hail_*` markers. Exact
ordinary static/shared symbol checks passed; no assertion was suppressed or
sanitizer marker exported as public API. A CTest-launched Windows PowerShell
observer initially inherited PowerShell 7's module search path and could not
load Get-FileHash; explicitly importing its own runtime's utility module fixes
the test environment. Both original failures and final checks are retained.

### Bounded Windows rerun observations

The initial follow-up campaign stopped after successful conditioning because
of E4: its recorded exit code was null; neither qualification case ran. It is
retained separately. After the device-free observer regression and fix, a fresh
predeclared campaign executed each historical configuration once, with no retry
of either qualification case. Both F32 cases passed the unchanged oracle.
The redirected-handle failure also matches the separately documented
[PowerShell process-observer issue](https://github.com/PowerShell/PowerShell/issues/5421).

| VB-CABLE profile, 48 kHz stereo F32 | 8 slots / requested 3 ms | 32 slots / requested 7 ms |
| --- | --- | --- |
| Sustained phase | 5.0156 s | 5.0080 s |
| Correlated delivery-age blocks | 1009 | 1008 |
| Delivery age median / P95 / P99 bucket upper bounds | 22.3 / 31.6 / 32.8 ms | 21.9 / 30.8 / 32.0 ms |
| Observed delivery age min / max | 10.2573 / 34.7467 ms | 9.8436 / 32.8675 ms |
| Flush acknowledgement, one trial each | 23.7487 ms | 26.1471 ms |
| Last old-marker capture read after request | 103.8174 ms | 107.3581 ms |
| Sustained capture loss / underrun / reference loss | 0 / 0 / 0 | 0 / 0 / 0 |
| SDK capture fractional frames | 0 / 494,880 | 0 / 495,840 |

Histogram bins are 100 microseconds; these are bucket bounds, not exact
quantiles or calibrated device input latency. Host correlation uncertainty
remains as the backend reports it; there is no physical confidence bound.
Requested delay is not actual scheduling delay (17.8070 / 18.8606 ms observed).
Deliberate starvation later in each test produces correctly observed loss and
underruns; those must not be confused with the zero-loss sustained phases.
Capture epoch remains continuous during cancellation; old selected records keep
their generation. SDK render/capture traces and zero PCM errors support software
generation isolation in these trials, not guarantees about post-mixer output.

S16 SDK capture still altered digital silence during conditioning (17,499 of
42,816 frames in the first completed campaign condition, maximum one S16 unit).
The F32 short-run passes therefore do **not** resolve the original numerical
qualification blocker. Defaults and the Shure microphone's gain/mute readbacks
are unchanged. No new physical microphone acquisition was used for these trials.
Input/output physical latency, calibrated round trip, callback deadline
distributions, physical drift/reference alignment, last presentation and
electrical/acoustic tails remain **unmeasured**, with the procedures above still
required for every supported physical profile. One flush observation per profile
does not establish P95/P99 acknowledgement behavior or a worst-case bound.

No public header or exported function changed in this follow-up. The optional
400-byte legacy info prefix and 408-byte current record remain as previously
implemented. Installed static/shared consumers and exact-symbol checks verify
the exercised configurations; 32-bit, alternate ABI/toolchains, arbitrary
physical channel maps and full historical binary compatibility remain ABI
readiness limitations. ABI remains provisional.

Audit base: `42537e2`, 2026-11-19. **Gate E verdict: PARTIAL.** Executable public
DSP interoperability and focused implementation fixes are delivered. Calibrated
physical latency, reference alignment, audible tails, target-device drift and
deadline behavior are not established. Existing failed WASAPI paired transitions
remain failures. This is not blanket production acceptance or an ABI freeze.

The task's `hl_` spelling differs from the implemented `ha_`/`HA_` API. This pass
preserves Hail's established namespace, exact export set and architecture.
Hail moves audio; a separate DSP processes it; inference handles speech; Hull
owns conversation policy. Keel and all these components remain optional clients.

## A–B. Source-grounded capability and gap audit

Read README, architecture/API/backend contracts, public/private headers, every
adapter, core/queue/native handoff, CMake/install/export checks, relevant portable,
native/fault/concurrency tests, fuzz harnesses, workflows, gate criteria and
qualification reports. Git history includes portable core `5921c55`, native
adapters `7b7aed1`, ownership/retirement separation `4e2d61f`, and the later
Windows SDK-boundary evidence. Proposal sections in architecture/verification
are not treated as implemented facts. Existing reports remain historical.

| Requirement / proposed work | Classification before this pass | Source / evidence and disposition |
| --- | --- | --- |
| Bounded PCM copies and SPSC ownership | Already implemented and adequately verified in portable scope | `queue.c:hail_ring_push/read`, `core.c:read_pcm/ha_stream_write`; conformance and public concurrency test. Preserve. |
| Independent duplex and rates | Already implemented and adequately verified in portable scope | `ha_stream_info`, per-leg gates/queues, `virtual_clock_info`; native scope varies. Preserve and exercise externally. |
| Clock identity, epoch, loss, validity, uncertainty | Implemented but insufficiently physically verified | `native.c:native_block/native_clock`, `ha_block_info`, virtual rational clocks; clarify domains and unknown uncertainty. |
| Drift observations / compensation | Metadata implemented; compensation intentionally out of scope | Consecutive frame/time anchors permit an estimator; no synchronization or resampler in Hail. Fixture checks both drift signs. |
| Event-to-stream identity | Partially implemented (G1) | `core.c:hail_event` emits private `s->id`, but old get_info omitted it. Optional public info tail now supplies the matching ID. |
| Multichannel arrays | Missing (G2) | `queue.c:hail_format` rejected all counts other than 1/2; native scratch assumed at most eight bytes/frame. Add bounded discrete transport and sized scratch. |
| Arbitrary physical channel maps | Partially implemented | Named application mono/stereo and indexed discrete channels now explicit; arbitrary spatial maps/array geometry still require an external device profile. Do not invent positions. |
| Render reference | Implemented but insufficiently physically verified | `queue.c:hail_render` copies selected PCM, including zeros, before SDK submission; AEC timing/alignment still needs calibration. No duplicate reference API. |
| Post-mixer/acoustic reference, exact presentation receipts | Intentionally unavailable in current portable API | SDK conversion/mixer/effects occur later. Existing reference never promises physical delivery or successful downstream write. |
| Generation/cutoff and native flush | Already implemented; native qualification incomplete | `core.c:ha_stream_flush_playback/progress`, all adapter flush methods; retain known WASAPI numerical failures. |
| Bounded cancellation acknowledgement | Implemented logical admission; hard time bound missing | Native SDK calls, worker scheduling and owner polling can delay acknowledgement. Publish measured per-profile distributions; never equate timeout with safe reclamation. |
| PipeWire render estimate | Partially implemented (G3, medium correctness) | `stream_process` substituted zero for invalid queued/buffered depth. Invalid depth now removes the host estimate; callback regression covers it. |
| CoreAudio malformed rate validation | Partially implemented (G4, low defensive safety) | `ca_native_format` could cast NaN to integer. Explicit finite check now precedes the cast. Source-confirmed malformed-input path; no physical occurrence claimed. |
| Independent public DSP integration | Missing (G5) | Existing tests covered pieces, but no separately configured installed-package DSP consumer. New executable closes that verification gap. |
| Endpoint pinning / explicit recovery | Already implemented, native coverage incomplete | Core selectors/registry, `hail_fail`, begin/finish retirement; public fixture now exercises loss, denial and reopen with new identities. |
| Hotplug, sleep, service restart, actual rate change | Implemented error/reopen policy; insufficiently verified | Native adapters reject changed formats/fail streams; full hardware matrix and sleep/resume evidence remain open. |
| Measured input/output/physical round trip and callback deadlines | Implemented telemetry in selected harnesses; insufficiently verified | Existing qualification tools/reports distinguish estimates/software loopback. No fabricated portable latency query added. |
| Native period, calibrated device latency and instantaneous frame occupancy queries | Partially implemented / unavailable publicly | get_info exposes application quantum/capacities; blocks expose timing estimates; status exposes loss/underrun counters. Native periods are harness SDK observations, not public device-latency guarantees. |
| AEC/NS/AGC/VAD/resampling/inference/service policy | Intentionally out of scope | No such dependency or processing enters core; fixture gain/channel selection is external mock processing. |

G1 is fixed by extending the existing get_info record, not by another identity
object/query. G2 is fixed within existing format/planner/adapter seams. G3 and G4
are source-confirmed defects; only G3 has an executed native seam regression in
this pass. G4's invalid-SDK-input test remains a platform follow-up. G5 exercises
actual library queues, clocks, lifecycle and virtual driver, not private mocks.

## C. Implementation and contracts

* `include/hail.h`, `queue.c`: 1..32 interleaved S16/F32 channels;
  `HA_LAYOUT_DISCRETE` preserves indexed channels without guessing physical
  positions. Explicit invalid-layout/zero/oversized/planar regression cases.
* `backend_wasapi.c`, `backend_coreaudio.c`, `backend_alsa.c`: scratch derives
  from validated format/quantum or maximum native slice through existing pure
  planning hooks, within the original one-stream allocation. No RT allocation.
* WASAPI discrete channel mask and PipeWire unpositioned negotiation avoid
  fabricating a stereo/speaker map. ASBD count alone no longer implies a
  physical channel layout. Actual native multichannel devices remain unqualified.
* `core.c:ha_stream_get_info`: append optional `stream_id`; original version-1
  prefix stays accepted, unknown tails/header preserved, no partial optional
  field write. IDs are context-scoped and match events.
* `examples/dsp-consumer/`: independent installed `Hail::hail` consumer. DSP
  module includes only `hail.h`; scenario driver includes public `hail/virtual.h`.
  No private include/source/test seam or new core dependency.
* CMake installed-consumer checks and general/native Linux CI now build/run that
  fixture for static/shared packages. Windows validation does the same.

The [API contract](api.md#external-dsp-ownership-identity-and-flow) defines pointer
lifetimes, alignment, identities, bounded flow, cancellation and reference
history. The [backend timing/provenance table](native-backends.md#timing-and-reference-provenance-for-external-dsp)
distinguishes application admission, selection, SDK submission, presentation
estimates and acoustic output. The [consumer instructions](../examples/dsp-consumer/README.md)
provide the executable boundary.

### Real-time and concurrency review

CoreAudio `ca_audio` validates at most HA_MAX_QUANTUM (8192) native frames and
splits into bounded application quanta; PipeWire `stream_process` likewise caps
frames and validates offsets/strides before copies. WASAPI capture caps each
packet and 64 packets per wake; render caps total available SDK capacity. ALSA
services at most one application quantum per leg per worker iteration. Virtual
steps at most one quantum per leg. Channels now cap at 32, four bytes/sample;
maximum single native slice is 1 MiB. Plans check arithmetic and quota before
allocation. More channels can exceed a device/byte/CPU budget and must not be
treated as an unconditional low-latency profile.

Hail audio paths contain no application callback, heap allocation, queue wait,
filesystem/network/GPU operation or control mutex. SPSC release/acquire publication,
single-attempt role gates, per-leg snapshots and terminal-error payload publication
remain unchanged. Callback counters are atomic. Stop/reset requires exclusive
roles; final free requires SDK retirement plus application quiescence. CoreAudio
SDK callbacks, PipeWire data-loop removal and WASAPI/ALSA worker joins retain
different native proofs. Control/SDK operations can block; ALSA xrun recovery is
not a hard-deadline path. Bounds on Hail's work do not prove SDK internals or OS
scheduling are realtime safe. Sanitizers do not prove deadlines or race freedom.

## D–E. Executed verification and platform status

Current raw checks are retained in the Gate E evidence package. Portable
regressions validate data/metadata, not only return codes. The external fixture
checks exact gain/PCM, clock slope, loss/backpressure, obsolete generations,
capture continuity, epoch changes, event identity and legacy record prefix,
default pinning, permission denial, device replacement, rate change through
reopen and pending native retirement. Four profiles include 700 modeled seconds;
this is not 700 seconds of hardware execution. Existing public concurrent
reader/writer/reference/flush/close and native handoff stress remain separate.

[Structured results](evidence/gate-e-2026-11-19/summary.json) and
[raw logs, source hashes and source snapshot](evidence/gate-e-2026-11-19/raw.zip)
retain failures alongside passing runs. Cross-platform implementation CI at
[`c76b497`](https://github.com/artalis-io/hail/actions/runs/44537959114) passes all
11 jobs, including native Linux static/shared loopback and x86 TSan; its
[device-free fuzz campaign](https://github.com/artalis-io/hail/actions/runs/44537958975)
also passes. The final regression additions are at `87e29d4`, independently
checked locally as below. Their
[full 11-job CI run](https://github.com/artalis-io/hail/actions/runs/44539247458)
and [fuzz run](https://github.com/artalis-io/hail/actions/runs/44539247463) also
pass; both revisions' evidence is retained separately.

| Local executed check | Result |
| --- | --- |
| Windows static Debug/Release | 13/13 device-free cases each |
| Windows shared Debug/Release | 9/9 device-free cases each |
| Installed C/C++/external DSP consumers | 3/3 in each of four Windows configurations |
| Matching LLVM 23 Windows ASan | 13/13 plus external installed consumer 1/1 |
| Linux ARM64 portable Debug | 7/7 plus external installed consumer 1/1 |
| Linux ARM64 GCC ASan/UBSan | 7/7 plus external installed consumer 1/1 |
| Linux ARM64 native compile/device-free seams | 9/9; ALSA/PipeWire enabled |
| Linux ARM64 native ASan/UBSan | 12/12, including actual libasound null 2/4/32-channel duplex |
| Clang static analysis | Ten invocations, no diagnostics |
| Old-header client | Original 400-byte info prefix preserved; tail canaries intact |

Windows commands use `tools/check_windows_builds.ps1`; the ASan installed
consumer uses the matching LLVM 23 compiler and runtime PATH. Linux uses an
isolated `/tmp/hail-gate-e.*` source/build tree, with Ubuntu dev packages extracted
there rather than installed system-wide. Exact shell/compiler commands and
artifact hashes are retained. Native ASan CTest explicitly excludes the private
PipeWire server test on that node; this is distinct from CI's executed server
loopback. ARM64 TSan's failed initialization is not counted as a pass.

| Platform | This pass / remaining limit |
| --- | --- |
| Virtual | Four external consumer profiles and portable regressions; zero-drift same-cadence model still reports conservative independent domains, not a fabricated shared-hardware clock |
| Windows WASAPI | Static/shared Debug/Release, exact exports and installed C/C++/DSP consumers; SDK buffer/retirement/oracle tests; LLVM 23 ASan. No fresh physical or paired numerical qualification |
| Linux ARM64 | Isolated Spark portable build, installed public consumer and GCC ASan/UBSan. Development headers extracted into the temporary workspace; no system package, device or inference-service change |
| Linux PipeWire / ALSA | ALSA 1.2.11 and PipeWire 1.0.5 compile; 9/9 device-free native/portable tests pass. Native ASan/UBSan passes 12/12 including real ALSA null duplex at 2/4/32 channels. These are not physical-device tests |
| macOS CoreAudio | Source reviewed; general CI compiles the adapter and runs portable/installed consumer checks. Fresh physical timing/array/spatial-map/error-input tests remain open |

An initial Windows ASan concurrency launch stalled while loading an ASan DLL
from the Visual Studio toolchain path. That owned process was terminated and
the failed run retained. Selecting the matching LLVM 23 runtime first on PATH
passes the suite and external consumer. The initial run is not counted as a
passing sanitizer check. See evidence for exact commands/results and CI scope.

The Spark GCC TSan binaries fail during runtime initialization with `unexpected
memory mapping` (7/7, before test execution). No ARM64 TSan success or race-freedom
claim is made; no ASLR/security setting or suppression was changed. The failed
run is retained separately from ASan and supported CI TSan evidence.

An old-header client compiled using `hail.h` from `42537e2` successfully reads
the 400-byte original stream-info record from the new library with tail canaries
intact. This verifies that particular prefix compatibility, not a frozen ABI.
Ten Clang analyzer invocations complete with no diagnostics. The external
consumer's compiler commands resolve installed headers only.

## F. DSP readiness

| External capability | Hail contract | Verification / blocker |
| --- | --- | --- |
| AEC | Capture/reference PCM, independent timing/domains, epochs/gaps/generation and provenance implemented | Public boundary exercised; physical alignment, uncertainty calibration and echo performance blocked |
| NS / AGC / VAD | Copied continuous PCM and explicit loss/reset metadata implemented | Mock processing/flow verified; algorithms intentionally external |
| Resampling / drift compensation | Per-leg formats, frame/host correlations, rational interpolation and uncertainty implemented | Mismatched-rate/drift metadata verified; no production resampler or physical drift calibration claimed |
| Channel processing | Mono/stereo and indexed 1..32 discrete channels implemented | 32-channel virtual/WASAPI seam copies and real ALSA null duplex verified; physical maps/arrays remain incomplete |
| Continuous ASR | Bounded capture transfer, gap/loss detection and external aggregation possible | Public processor/output backpressure verified; no ASR/model runtime added or exercised |
| Incremental TTS | Bounded partial writes, selected-reference progress, underrun zeros and generation rejection implemented | Public duplex/cancellation and threaded tests; exact hardware presentation progress unavailable |
| Full-duplex interruption | Logical cancellation and native reset preserve capture; shutdown retirement remains separate | Virtual/native seam evidence; physical audible cessation and failed WASAPI paired transparency block full acceptance |

## G–H. Remaining qualification and acceptance

Architectural boundary is preserved; no DSP graph, model awareness, mandatory
Keel/Hull dependency or second lifecycle/clock/buffer abstraction was added.
Remaining architectural limitations are incomplete arbitrary physical channel-map
reporting, no portable presentation receipt/post-mixer tap, and uncalibrated native
uncertainty. Remaining implementation risk includes SDK error/reconfiguration
paths without exhaustive target fault coverage. Hardware gaps and performance
uncertainty are tracked separately below. ABI stays provisional: struct-prefix
tests and unchanged exports do not establish full old-binary compatibility,
32-bit/endianness coverage, platform minimums or sibling namespace acceptance.

Use the existing platform qualification suites, not this virtual consumer, to
close Gate E:

1. Record revision/compiler/binary hash, OS/CPU/device/driver/transport, clocks,
   nominal/native/application rate, channel layout, queue capacity and period,
   power/load configuration, test duration and raw logs. Predeclare profiles
   and thresholds from an actual baseline. Do not retry failed trials into passes.
2. Windows: follow [physical execution](native-windows-physical-results.md) and
   [paired qualification](wasapi-qualification.md). Run the existing
   `run_wasapi_qualification.ps1` against an approved manifest in a fresh output
   directory. It explicitly conditions silence and retains numerical failures;
   no profile can pass by weakening the exact oracle. The VB-CABLE alteration
   remains unresolved.
3. macOS: follow [CoreAudio qualification](coreaudio-qualification.md) and
   [physical follow-up](native-macos-followup.md). Keep the 2048-frame hosted
   functional profile separate from failed low-period profiles. Use diagnostic
   callback/control histograms only in their instrumented build and read them
   after retirement.
4. Linux: build with both native backends and HA_BUILD_NATIVE_TESTS. Run the
   [private PipeWire runner](loopback-qualification.md) for exact software
   loopback; run `hail_alsa_null` separately for null-device lifecycle. Then
   repeat on explicit physical endpoints using the native sample/fixture and
   an approved cable/acoustic measurement path; null/monitor success cannot
   qualify a physical microphone/speaker or calibrated latency.
5. For each physical profile, perform at least ten minutes steady duplex,
   both-leg and separate-device trials, positive/negative observed drift,
   array-channel identity checks, and repeated flush/restart. Use calibrated
   known PCM/impulse correlation to distinguish capture delivery age, output
   queue estimates, physical round trip, reference offset and audible tail.
   Measure each separately; retain median/p95/p99/max, sample counts, uncertainty
   and startup/transitions separate from nominal loss/underrun counts.
6. Exercise unplug/replug, actual permission denial/revocation, busy/unsupported
   formats, default pinning, service restart, sleep/resume and rate changes.
   Require terminal/reopen behavior with fresh identity/domains, balanced
   ownership and no stale playback. Hold application readers during close;
   join them before final destruction. A watchdog records failure and retains
   ownership; it never authorizes freeing a live callback.

Acceptance thresholds must name the hardware/profile. Nominal selected profiles
should have zero unexplained PCM errors, stale-generation playback or unreported
loss; deliberately induced gaps must be reported exactly. Choose callback CPU
and jitter budgets relative to the measured native period, and capture/reference
alignment and acoustic-tail targets from the DSP/product requirements. No
universal submillisecond flush, sample-perfect independent clocks or finite
uncertainty is claimed. **Gate E remains PARTIAL until this physical evidence
and the remaining contract limitations are resolved or explicitly scoped.**
