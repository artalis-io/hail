# Gate E: full-duplex speech and DSP readiness

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
