# Hail verification and implementation gates

Status: architecture approved; Gate C portable core and virtual backend are
implemented, and Gate D adds CoreAudio, shared WASAPI, PipeWire and ALSA. Native
implementation is complete; full physical-platform qualification remains open. The matrix below remains the full verification
plan, not a claim
that every planned case is covered. Current executed checks are recorded in the
next section. Sibling namespace inspection remains in the [audit](audit.md) and
[raw evidence](evidence/namespace.json). Physical latency remains unmeasured; software-loopback measurements below have a separate scope.

Source hashes, tool versions and per-target results are recorded in
[native evidence](evidence/native.json).

## Current executed evidence

Owner-authorized [Shure Windows execution](native-windows-physical-results.md)
now passes 32 strict duplex format/startup cases, standalone capture/playback,
all static/shared Debug/Release samples, 30 native lifecycle cycles and a
600.009 s strict duplex phase with zero sustained xruns or capture continuity
changes. A real converted-capture clock-fact defect was fixed; all failed
lower-rate cases are preserved. Matching LLVM 23 ASan passes device-free and
native trials, including an independent intentional-overrun runtime check.
Owner-coordinated real USB loss/reopen passed with fresh endpoint IDs/clock domains
and balanced teardown. No microphone PCM was retained. The installed
[VB-CABLE pair](native-windows-paired-results.md) then failed all four strict
48 kHz stereo PCM cases. Controlled paired-fixture correctness, physical
timestamp accuracy and calibrated latency remain unqualified.

On 2026-11-18, native Windows ClangCL static Debug/Release each passed nine
device-free CTests; shared Debug/Release each passed five, and all four installed
C/C++ consumer configurations passed. PE/COFF symbol checks matched the 27 public
exports. New WASAPI SDK seams cover packet leases/flags/bounds, stop/flush/start
failures, command supersession, notifications and partial initialization. The
native harness's independent oracle passes corruption-negative tests. Actual
physical/cable streams were not opened. ClangCL 19 ASan built but failed before
test execution during runtime interception on Windows build 26300. See
[Windows results](native-windows-results.md) and [harness setup](wasapi-qualification.md);
these results do not complete the native Windows goal.

The expanded [device-free fuzz suite](../fuzz/README.md) adds separate
configuration, lifecycle, queue-model and backend-failure harnesses. On macOS
arm64 with Homebrew Clang 22.1.8, each completed 100,000 seeded inputs with
ASan/UBSan, seed 73 and maximum input length 1,024. The normal macOS build's
eight device-free CTests also passed. These campaigns cover the portable core,
virtual adapter and scripted backend seam; they do not qualify native SDKs or
prove all thread schedules safe. The new fuzz CI workflow runs bounded campaigns
on pushes/PRs and longer weekly campaigns, retaining logs and reproducers.

The published audit/separation fixes at `4e2d61f` passed all eleven jobs in
[CI run 44171083719](https://github.com/artalis-io/hail/actions/runs/44171083719).
Windows static executed all seven CTests, including `wasapi_retirement` and
`backend_axes`; Windows static/shared installed consumers passed separately.
These device-free tests establish fault/ownership behavior, not WASAPI streaming.
The complete CoreAudio fixture workflows also passed on
[macOS 15](https://github.com/artalis-io/hail/actions/runs/44171131139) and
[macOS 26](https://github.com/artalis-io/hail/actions/runs/44171136350): each ran
40 strict native loopback processes and three ASan/UBSan processes at the
declared 2,048-frame fixture period. The [evidence snapshot](evidence/audit-ci-2026-11-17.json)
records the checked source and artifact hashes. See the
[C audit resolutions](c-audit-2026-11-17.md) and [axis audit](axis-audit.md).
Native Windows physical/duplex qualification remains outstanding.

On Linux x86-64 with GCC 13.3 and Clang 18.1.3, the portable CTest executables
cover C/C++ record boundaries, eight grouped deterministic conformance scenarios,
concurrent capture reader/playback writer/reference reader with flush/close,
and the fixed-arena duplex example. The conformance scenarios include format/rate
combinations, capture/reference loss, silence, stale generations, injected
start/stop/flush/permission failures, delayed retirement, device replacement,
default pinning, independent streams, rational drift/partial reads, ring-index
wraparound, allocator failures and exact resource plans. These tests do not
constitute exhaustive state-space/model-checking coverage.

Passed configurations: GCC debug static, GCC release shared, hosted-allocation
exclusion with virtual tests, Clang C with GCC C++ consumers, GCC ASan/UBSan
(including LeakSanitizer outside the sandbox), and Clang ThreadSanitizer.
GCC TSan failed before program execution with unexpected memory mappings;
Clang's runtime passed after selecting the installed GCC 13 C++ libraries.
No sanitizer finding was suppressed. Clang static analysis reported no findings.

Linux symbol checks verified the exact 27 public functions and hidden shared
helpers. PE exports and Mach-O public definitions match the same 27 functions. A virtual-disabled freestanding compile exposes no backend and has
only memory helpers plus internal references as undefined symbols. Installed
static/shared CMake consumers pass separately in C and C++; relocated pkg-config
consumers also compile/link/run for both library kinds. A Clang libFuzzer
ASan/UBSan run completed 30000 inputs with seed 73 and maximum length 96 without
a finding. The fuzz target exercises configuration geometry and bounded virtual
operation sequences, not malformed pointers or a native driver.
Gate D commit 7b7aed1 passed all eleven macOS/Windows/Linux jobs in
[CI run 43917043569](https://github.com/artalis-io/hail/actions/runs/43917043569).
This includes native target builds, context creation/enumeration on macOS and
Windows (headless snapshots may be empty), portable tests and installed consumers.
These checks do not open native microphones or prove physical stream behavior.
The follow-up fixture commit b6a97f9 has passing Linux static/shared native suites
in [run 43919570772](https://github.com/artalis-io/hail/actions/runs/43919570772).
At the [saved CI snapshot](evidence/qualification-ci.json), ten jobs pass and the
macOS shared job is waiting for a runner. Native source/public headers are
unchanged from the fully green baseline; the remaining job is not reported as a
completed check.

Gate D local checks add a native producer/control handoff stress test and an
opt-in real libasound null-device duplex test (six tests total in native static
builds). All six pass under GCC ASan/UBSan/LSan and Clang TSan. These cover native
gate/snapshot publication, clock reset, device removal during producers, context
retirement retry, real null PCM start/stop/restart and capture-preserving native
flush. Actual PipeWire 1.6.8 RT_PROCESS callbacks also ran duplex against the
Dummy Output sink/monitor, including flush and continued capture, under Clang
TSan without findings. No physical microphone was opened and no audible PCM was
submitted. Those initial smoke exercises did not measure latency.

Follow-up qualification adds a real, isolated PipeWire null-sink/monitor loopback:
seven native static CTests now pass under ASan/UBSan/LSan and Clang TSan. The new
test checks nonzero PCM exactly, generation order, capture-preserving native
flush, stale writes, disappearance, replacement/reopen, deliberate capture
starvation and playback underruns, and balanced destruction. Release software
round-trip and flush acknowledgement measurements are recorded in
[the loopback report](loopback-qualification.md); they do not establish physical
latency, hard deadlines or long-duration stability.

The separate PipeWire ASan exercise reports 3525 leaked bytes in 34 allocations
from context module initialization. A [minimal reproducer](../tools/probes/pipewire_context.c)
with no Hail code reproduces the same result after context destruction and
pw_deinit. No leak suppression or disablement was used. This is an unresolved
native dependency finding, not a passing leak qualification for PipeWire.
Reproduce with the system PipeWire pkg-config flags and `-fsanitize=address`.

Cross builds compiled and linked CoreAudio arm64 static and shared Mach-O with
Clang 18 and the macOS 11.3 SDK, and shared/static WASAPI x86-64 PE with MinGW-w64. Both native C examples
also cross-link against their static libraries. The cross-host Mach-O archive
requires LLVM ranlib to generate its target index.
The SDK cross builds establish no execution evidence; the CI run above adds
actual target build/enumeration coverage, still without physical stream tests. ALSA 1.2.11/PipeWire 1.6.8 static/shared builds and
installed C/C++ consumers are checked separately on Linux. Native callback
allocation audits, failure/permission/hotplug qualification on each target and
measured latency remain explicit D/E qualification work.

CoreAudio now has native BlackHole static/shared loopback execution and a separate
instrumented ASan/UBSan profile, with earlier timing-sensitive failures retained.
See [CoreAudio qualification](coreaudio-qualification.md). Windows WASAPI now has
the separate [Shure physical execution](native-windows-physical-results.md),
including sustained duplex and USB loss/reopen. A signed paired-endpoint fixture
is still required for the [controlled loopback suite](wasapi-loopback-design.md).

## Gate completion criteria

| Gate | Deliverable | Required evidence before completion |
| --- | --- | --- |
| A repository audit | Source-grounded ecosystem/backend comparison and namespace inventory | Available repository findings are delivered; Sail and actual Hull exports explicitly remain evidence gaps, blocking final namespace acceptance |
| B architecture | [Approved contracts](architecture.md) | Owner approved on 2026-11-15 |
| C portable core | Public header, resource planner, state machines, queues, virtual backend, examples | Deterministic conformance, fault allocation, teardown schedules, portable builds, sanitizer runs, header/ABI checks |
| D native backends | CoreAudio, WASAPI, PipeWire, ALSA | Target compile/link/functional/hardware evidence separately recorded; no platform claim based on another host's virtual tests |
| E real-time validation | Measured performance and deadline behavior | Reproducible configurations, raw data, clock/latency method, callback allocation audit, stable duplex interruption tests |
| F integration readiness | External `hull:audio` adapter specification | Capability, quota, cancellation, thread and teardown plan; no binding code without separate authorization |

Production acceptance requires all implementation gates, not merely an
architecture document. A backend whose required operation is unsupported must
return an explicit error and cannot pass the corresponding required conformance
case by skipping it.

## Deterministic virtual backend

Implement before completing physical adapters. The driver is externally
stepped: no wall clock, sleeps, background threads, actual device permissions or
hardware. `hail/virtual.h` provides a versioned scenario config, bounded event
script, step operation, capture source injection, and playback inspection.
Scenario buffers and output inspection queues have explicit capacities and
overflow errors. Test sinks must never silently truncate inspected playback.

Each step supplies a logical scheduling point and bounded capture/render frame
requests. Capture source can be a supplied fixed PCM pattern or deterministic
frame/channel counter function. An independent reference oracle predicts exact
samples and continuity; it must not call production queue helpers to compute
expected results. The virtual endpoint can request unequal periods and rates
for each leg. Playback inspection records selected samples, silence,
generation, position and modeled presentation time separately.

Virtual host time uses checked rational arithmetic. Each device has a separate
rational clock ratio, positive/negative configurable drift, start offset,
timestamp validity and uncertainty. Crossing an injected reset starts a new
epoch. Fractional accumulators avoid floating-point drift in the test oracle.
Model a bounded native submission pipeline so software flush and native reset
have observably different audible tails.

| Injection | Deterministic observation |
| --- | --- |
| Capture consumer delayed | Queue fills, newest frames lost, exact cumulative loss and later gap metadata |
| Playback producer delayed | Exact silence frames, render timeline advance, underrun counts |
| Independent clocks drift | Expected position/host correlation slope; no implicit synchronization |
| Permission denied at probe/open/start | Correct portable result, no leaked unpublished resources |
| Failure after each start/stop phase | Both legs retired, one terminal result, no leftover running leg |
| Device disappears/reappears | Terminal loss, new incarnation, explicit reopen and empty new queues |
| Default changes | Notification; old endpoint remains pinned or explicit loss if it cannot |
| Flush before/during/after render | Old generation cutoff, acknowledgement order, capture continuity |
| Native reset delayed/fails | Pending status or terminal failure; no premature successful flush |
| Host/device clock jumps or validity loss | New epoch/invalid correlation, observable uncertainty |
| Delayed callback after cancellation request | Storage retained until scripted retirement |
| Inline completion | No premature reclamation or recursive unbounded progress |
| Full event/reference queues | Observable loss, sticky terminal/flush completion survives |
| Malformed backend buffer/metadata | Bounded validation failure, no out-of-range access |

Expose internal test scheduling barriers at admission, payload publication,
command consumption, native submission, callback unwind, retirement and final
free. Single-thread schedule enumeration proves state ordering deterministically;
actual multithreaded tests with barriers exercise the C memory model separately.
Do not pretend a single-thread virtual script proves data-race freedom.

## Portable conformance matrix

| Area | Cases and oracle | Tools/evidence |
| --- | --- | --- |
| Discovery | Empty list, input/output/default roles, insufficient caller capacity, stale snapshots, long/invalid IDs, registry cap, replacements, defaults | Copied records, incarnation uniqueness and transactional refresh oracle |
| Negotiation | S16/F32, mono/stereo, 16/24/44.1/48 kHz, unequal duplex formats, native/server unknown format, explicit conversion modes, unsupported layouts | Exact negotiated records and portable failure codes |
| PCM transfer | Capture/playback/duplex, partial slots, partial write, zero frames, ring wrap near UINT32_MAX, oversized native quantum | Sample-for-sample oracle, guarded destinations |
| Flow control | Slow reader, fast writer, full queues, silence remainder, reference overflow | Exact frame/loss counters, no producer writes into reader-owned slots |
| Lifecycle | Every legal and illegal state/operation pair, repeated start/stop, stop during start, close during each transition, failure rollback | Transition table and postcondition assertions |
| Flush | Empty/full queues, partial render slot, concurrent admitted writer, stale delayed writer, overlapping flush, stop/close supersession | No old Hail submission after ack, capture sequence unchanged, completion survives event loss |
| Recovery | Loss on either leg, replacement/default change, permission revoked, native restart | No automatic retry, new stream/domain identity, no old buffers replayed |
| Memory | Failure at each allocation, byte/alignment cap, fixed arena, partial init rollback, context destroy with live streams | Alloc/free ledger, canaries, ASan/UBSan/LSan |
| Pointer lifetime | Caller buffers reused immediately after copy, native buffer expires after processing, delayed native completion during close | Poisoned buffers and deterministic retirement fixture |
| Destruction | close_begin versus read/write, app join then finish, context notification race, inline completion, NULL-slot repeated finish | No post-retirement access, exactly-once free; stale raw aliases documented as invalid |
| Timing | Continuity, known/unknown gaps, silence positions, epoch reset, independent drift, valid timestamp zero, uncertainty, rate arithmetic overflow | Rational oracle; monotonicity only within valid continuous epochs |
| Bounds | Null pointers, short structs, unknown versions/required bits, extreme lengths/rates/channels/strides, generation/epoch exhaustion | Fuzzed configs/backend descriptors; no unbounded allocation or loops |
| Resource independence | Multiple streams and contexts, close one while others run, allocator budgets isolated | No global mutable cross-instance state |
| ABI | Old/new record prefixes, output canary tails, fixed symbol allowlist, C/C++ consumers, static/shared, Windows call convention | Saved ABI fixtures per architecture, nm/readelf/dumpbin, combined sibling headers |
| Portability | Virtual-only hosted and freestanding builds; OS/thread/DSP header exclusion; no malloc in data-path call graph | Compiler warnings, structural dependency checks and undefined-symbol inventory |

Data APIs are bounded but application memory can still be invalid; fuzzers only
pass valid allocations with malformed declared geometry. Do not execute stale
pointer aliases as though the library promises to diagnose arbitrary freed
memory. Negative lifetime tests instead prove storage cannot be reclaimed while
a valid caller/backend lease remains.

## Concurrency verification

Use barriers, not sleeps, for write/flush admission conflicts, start/stop, reads
while close_begin revokes admission, device loss during processing and late
native notifications. Pause a writer after gate acquisition and prove flush
returns BUSY without changing generation; pause render inside native submission
and prove flush cannot complete until it unwinds. Join application roles before
final free and race native retirement with the last control poll. Exercise
multiple independent streams with distinct frame patterns to catch cross-talk.

ASan/UBSan target memory/arithmetic issues. TSan targets actual concurrent queue,
mailbox and lifecycle accesses on supported Linux/macOS toolchains. MSan is a
separate Clang lane only where all linked code can be instrumented. Check
generated atomics and reject RT use on a target where required operations call
a locking runtime helper. Native frameworks may need focused harnesses and
documented tool limitations; an uninstrumented driver cannot count as TSan
coverage. Run Clang static analysis and configuration/metadata fuzzing too.

## Platform support evidence

| Backend/target | Compile/link evidence | Execution evidence | Physical devices | Measured latency |
| --- | --- | --- | --- | --- |
| Portable core and virtual | Linux GCC/Clang static/shared, freestanding exclusion | Portable tests; ASan/UBSan and Clang TSan | Not applicable | Not measured |
| Linux PipeWire | Static/shared, GCC/Clang | Isolated nonzero loopback, flush, loss/reopen, starvation; ASan/UBSan/LSan and TSan clean in fixture; default-config dependency finding remains | Not tested | Software only; see loopback report |
| Linux ALSA | Static/shared, GCC/Clang | Null-device duplex, flush, stop/restart; ASan/UBSan/LSan and TSan clean | Not tested | Not measured |
| macOS CoreAudio | arm64 native macOS 15/26 static/shared; Intel portable/consumer CI | BlackHole full format matrix at explicit 2,048-frame native period; strict Release + ASan/UBSan; nominal/loaded physical duplex; aggregate loss/defaults; denied-open and rollback fixes; permission-worker ASan/TSan; [CI scope/failures](coreaudio-ci-regression.md) | Built-in M1 Max microphone/speakers at 48 kHz; actual OS denial passes; physical USB/Bluetooth hotplug unqualified; [follow-up](native-macos-followup.md) | Software flush/callback/delivery measurements; 512-frame hosted failures retained; physical latency unmeasured |
| Windows shared WASAPI | MinGW cross static/shared; ClangCL native static/shared Debug/Release and installed C/C++ consumers | SDK fault/oracle tests; 32 strict format/startup cases; 600 s zero-xrun duplex; 30 lifecycle cycles; real USB loss/reopen; matching LLVM 23 ASan; signed paired fixture pending | Shure MV7+ capture/headphones on Windows 11 x64; exact [scope and evidence](native-windows-physical-results.md) | Software control/timing observations only; future SDK correlations retained; physical latency uncalibrated |
| Embedded/RTOS/UEFI | Extension design; freestanding core compiles | No adapter | No adapter | Not measured |

Planned CI: GCC and Clang Linux, AppleClang macOS, MSVC and MinGW Windows; debug
and release; static/shared; C and C++ installed consumers; virtual-only and each
native feature; Linux sanitizers and target compile checks. Add backend-specific
failure mappings and native retirement tests. Native functional tests need
recorded endpoint/driver permissions and available hardware; headless tests
must label skips, not report them as device success.

Linux checks separately cover a PipeWire session, direct ALSA where permitted,
ALSA plugins, server absent, permission denied, server restart, default change,
USB removal and native xruns. macOS checks purpose string/authorization states,
HAL property changes and teardown listeners. Windows checks COM thread ownership,
event-driven startup, endpoint invalidation, default-role changes and render
reset preserving capture. Duplex must be exercised on a shared physical device
and separate independent endpoints where available.

## Benchmark methodology

Record CPU, memory, OS/kernel, audio backend/server/driver versions, physical
device and firmware, transport (built-in/USB/Bluetooth), power mode, scheduling,
format, rate, native periods, all queue capacities and conversion flags. Keep
raw traces and exact build revision/compiler/options with results. Begin with
16/24/44.1/48 kHz mono/stereo S16/F32 where endpoints support them; unsupported
requests are results, not substituted silently.

| Metric | Measurement |
| --- | --- |
| Capture to consumer | Native first-frame capture estimate to app read time; separately report uncertainty and queue residence |
| Producer to presentation | App write timestamp to correlated native presentation estimate; label estimates and confidence |
| Physical round trip | Wired loopback impulse/correlation with calibrated path; separate ADC/DAC/analog contribution from queue latency |
| Callback execution and jitter | Preallocated trace of callback entry/exit and expected period; format/log trace off RT |
| CPU and allocations | Process/thread CPU plus Hail allocator ledger before/after start; distinguish OS allocations from Hail calls |
| Memory | Resource planner reservation, allocator high-water mark and separately process resident memory |
| Duplex stability | Long continuous capture/render with exact xrun/gap counts and timing coverage, idle and load stress |
| Interruption | Flush request to software acknowledgement and separately last audible old sample through loopback; correlate generation/cutoff |

Report duration, sample count, median/p95/p99/max, xrun counts and missing timing
samples. Counter overhead belongs in method notes. Queue duration `frames/rate`
is a calculated capacity, not a measured latency. No hard real-time promise or
target threshold becomes a measured achievement without hardware evidence.
Determine product latency targets during review and publish actual results,
including failures. Optimize only after correctness and retirement invariants
pass.

## Documentation and examples

Architecture sections already specify ownership, threading, real-time safety,
states, clocks and ABI policy. The API reference and virtual/native C examples provide executable entry points for enumeration, capture, PCM playback,
duplex, generation-based interruption, disconnection/reopen, and virtual driving.
Examples must compile in CI against the installed header; signature sketches
in the architecture are not presented as compilable examples.

Backend authoring documentation must pin the private operation table, buffer
validation, producer serialization, native notification retirement, quantum
bounds, timing validity, native flush guarantees and terminal failure mappings.
Build/integration instructions must include minimal static/shared and
virtual-only invocations, exact optional dependency versions, toolchain files,
freestanding allocator/clock/atomic requirements, and no-network reproducible
source acquisition. Known limitations and the support matrix must reflect
tests actually run, not features merely compiled.

## Requirement traceability

| User sections | Gate B location | Implementation or evidence follow-up |
| --- | --- | --- |
| 0 and 1 naming/ecosystem | Audit provenance, namespace evidence, sibling findings | Sail/Hull binary audit and exact Hail exports before freeze |
| 2 independent axes | Architecture axis table, diagram, abstraction review | Structural checks, backend substitution conformance |
| 3 public C ABI | API shape, ABI evolution, pointer ownership | Frozen header/layout fixtures and C/C++ examples in C |
| 4 discovery | Devices/negotiation, explicit incarnation/default policy | Virtual registry cases, native notifications in D |
| 5 formats | Exact modes, app/native format and conversion validity | PCM/format matrix in C/D |
| 6 buffering | Memory formulas, drop-newest, silence, flush protocol | Queue/generation schedules and allocator ledger in C |
| 7 clocks | Domains, epochs, positions, uncertainty, drift | Rational virtual oracle then native correlation tests |
| 8 duplex | Independent legs, capture-preserving playback flush | Virtual conformance, physical duplex in D/E |
| 9 concurrency | Role topology, shared structures, memory ordering | TSan/barrier tests, target atomic proof |
| 10 and 11 lifecycle/errors | State table, begin/finish retirement, sticky errors | Exhaustive transitions, partial init and device-loss schedules |
| 12 platforms | Audit native evidence, backend decision | Native spikes, minimum versions and support matrix in D |
| 13 embedded | Freestanding boundary, arenas, external driving | Compile-only profile in C; no speculative firmware implementation |
| 14 DSP | External companions and render reference limitations | Consumer examples; no mandatory DSP dependency |
| 15 build | CMake target/options, visibility and dependency strategy | Install/static/shared consumers, license manifest |
| 16 virtual | Deterministic driver specification above | First backend in C |
| 17 verification | Conformance, concurrency, platforms, benchmark method | Record executed evidence per gate |
| 18 security/resources | Explicit caps, checked sizes, permissions, budgets | Config/metadata fuzzing and quota failure tests |
| 19 documentation | Architecture contracts plus documentation plan above | Compilable examples and final API/backend/build guides in C/D |
| 20 and 21 gates/acceptance | Gate table and honest support evidence | Owner architecture review, then C through F; all acceptance items remain implementation gates |
