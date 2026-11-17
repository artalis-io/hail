# Audio architecture axis audit

## 2026-11-18 Windows qualification artifacts

Applied [axis-audit](../.claude/skills/axis-audit/SKILL.md) to the WASAPI fixes,
native test, provisioning scripts and build/export checks on base `dbfbc27`.
The map and cross-backend traces in the preserved 2026-11-17 pass remain the
architecture baseline. No new upward cross-axis coupling was found in this
focused scope; this is not blanket native Windows qualification.

| Boundary/trace | Observed ownership and evidence |
| --- | --- |
| Capture transport | COM worker acquires/validates/releases packets; `hail_native_capture` publishes bounded copied PCM/time; application I/O thread reads public partial-slot records. SDK silence/timestamp facts now survive translation. Nonzero/silent/split/malformed packets pass device-free seams. |
| Playback and interruption | Application writes generation-bound PCM; worker selects through `hail_native_render`, returns native buffer, and owns render Stop/Reset/Start. Capture client/gate/clock remain independent. Failed/superseded commands cannot acknowledge success. Actual native continuity remains untested. |
| Lifecycle and memory | Application roles join before close_finish; native worker termination and notification retirement independently prove storage safety. New fault tests cover every partial event acquisition and failed thread creation. No timeout authorizes reclamation. |
| Fixture/discovery | Exact persistent keys and directions are validated against signed-driver/PnP records outside Hail. Provisioner scripts own installation/removal policy. Native loss mode requires DEVICE_LOST, explicit close/reopen and new incarnations/domains; it is compiled, not executed. |
| Timing/representation | Independent leg domains remain intact. Test-only SDK queries report mix/application/native-period facts; physical format remains unknown. Exact grid oracle is outside portable transport; conversion profiles need independent future numerical oracles. |
| Processing/service | No DSP, speech, network, driver installer, scheduler or fixture policy enters core/public headers. No public ABI change or mandatory new dependency. |

Windows static/shared Debug/Release, installed C/C++ consumers and PE/COFF
symbol/boundary checks passed. Static checks run source-private SDK seams; shared
checks run only public tests. The native oracle's negative cases also passed.
Clang analysis reported no diagnostics. ASan failed in runtime initialization;
no supported sanitizer or physical/paired audio run is claimed. The
[evidence/support matrix](native-windows-results.md) explicitly leaves sustained
I/O, actual COM streaming, driver loss/reopen, format conversion, latency and
memory diagnostics incomplete. A passing provisioned workflow will still be
virtual-cable evidence, separate from physical audio and audible latency.

Focused safety fixes are recorded in the [C audit](c-audit-2026-11-18.md).
Next acceptance work is to supply approved physical endpoints/level and signed
fixture manifests/procedures, execute the strict/sustained/loss trials, and retain
failures and actual measurements. Do not infer support from the harness itself.

## 2026-11-17 separation fixes

**A2 and A3 are resolved. All four findings from the initial pass are now
resolved within the reviewed scope.** The initial pass below is retained as
history, not a description of remaining implementation debt.

| Boundary | Change | Evidence |
|---|---|---|
| Identity versus execution | Every backend explicitly declares `HAIL_EXEC_SERIALIZED` or `HAIL_EXEC_CONCURRENT`; core rejects an unspecified mode. The cached stream `concurrent` flag derives only from that declaration. Backend identity and capabilities no longer select queue/counter ownership. | `backend_axes` exercises ALSA and virtual identities with each ownership mode, using the same PCM, loss, reference, partial-read, clock/gap, stop and terminal-error assertions. These are test adapters, not claims of actual ALSA/virtual scheduling combinations shipped to users. |
| Execution versus notification | Snapshot transfer is concurrent-only; atomic terminal-error mailboxes are collected for either audio ownership model. Capture-loss counters and block loss metadata select the appropriate owner rather than assuming concurrent execution. | The same four combinations verify cumulative loss metadata, capture discontinuities and device-error delivery. |
| Generic versus adapter storage | Virtual fault settings, delay progress, drift clocks and synthetic PCM scratch now live entirely in `virtual.c` private state. `ha_stream` has no virtual-only fields or scratch pointer. The virtual clock helper is file-local. | Equal capture/render rings with a fixed-size test adapter have equal memory plans. Maximum stereo F32 virtual capture separately reserves and exercises its 65,536-byte synthetic scratch region under ASan/UBSan. Native capture no longer pays that scratch cost. |
| Planning versus allocation | Fixed `stream_bytes` plus the optional pure `stream_extra_bytes` hook describe adapter storage; core checks addition/alignment and includes it in one stream allocation. Plans do not allocate or mutate adapter state. | Tests cover exact plan/open accounting, extra space, planner errors, size/alignment overflow, unknown execution mode, no steady-state allocations and balanced deallocation. Freestanding arena tests also pass. |
| Lifecycle versus virtual delay policy | Generic lifecycle code no longer resets virtual poll counters. The adapter tracks its operation and generation, retaining completed delay progress while application gates are still busy. | Repeated flushes, flush cancellation, repeated stop/restart, cancellation during STARTING and stop completion delayed by an admitted reader are deterministic test cases. |

`tools/check_contracts.py` now rejects concrete backend IDs in core/queue code
and virtual-only fields in the generic stream, in addition to its SDK and export
checks. These checks guard specific boundaries; they do not prove arbitrary
future code correct. Backend-specific virtual API validation and AUTO factory
selection still use identity for their intended selection purpose.

Validation of this pass:

| Configuration | Result |
|---|---|
| macOS arm64 static, device-free CTest | 8/8 passed |
| macOS LLVM ASan/UBSan and TSan, same selection | 8/8 passed each |
| macOS shared, device-free CTest and exports | 4/4 passed; 27 public symbols |
| Linux GCC static with ALSA/PipeWire | 11/11 passed, including software loopback and native fault tests |
| Linux Clang ASan/UBSan, same selection | 11/11 passed |
| Linux GCC shared with ALSA/PipeWire | 6/6 passed; 27 public symbols |
| Linux freestanding virtual / backend-free compile | 4/4 passed / compile passed; 27 / 22 public symbols |
| macOS/Linux Clang analysis of portable and applicable native sources | No diagnostics |
| Windows x64 cross-build, then hosted Windows ClangCL static/shared CI | Passed; static CTest executed `backend_axes` and `wasapi_retirement` (7/7 tests), plus 2/2 installed-consumer tests |

Published source `4e2d61f` passed all 11 jobs in
[general CI](https://github.com/artalis-io/hail/actions/runs/44171083719), plus
the complete CoreAudio workflows on
[macOS 15](https://github.com/artalis-io/hail/actions/runs/44171131139) and
[macOS 26](https://github.com/artalis-io/hail/actions/runs/44171136350).
Each CoreAudio workflow passed 40 strict loopback processes and 3 ASan/UBSan
processes with the declared 2,048-frame native fixture period. The
[CI evidence snapshot](evidence/audit-ci-2026-11-17.json) records job links,
Windows pass markers, platform details and source/artifact hashes.

The public headers and function signatures are unchanged; requested allocation
sizes correctly reflect the new private layout. No physical audio was opened.
Existing physical qualification evidence describes its original source/layout;
this pass adds transport, ownership and memory evidence without relabeling those
measurements. Native Windows duplex qualification remains the next platform
task, rather than an unresolved axis-separation finding. Hosted Windows fault
tests have now executed; they do not open native audio devices or qualify
Windows duplex operation.

## 2026-11-17 initial pass (before separation fixes)

Reviewed base `7da3188129dffd8c24056357ea1492e48f71c8b6` plus the C-audit
fixes in this working tree, using the adapted
[axis-audit skill](../.claude/skills/axis-audit/SKILL.md).

The public/portable boundary is sound in the paths reviewed: SDK resources and
scheduling stay in adapters; queues transport PCM without DSP/networking;
capture and render have separate ownership and clocks. This is not a claim of
full native-platform qualification. Two private implementation couplings remain
as low-priority cleanup items. The pass also found and fixed one medium
diagnostic defect and one low documentation mismatch.

### Architecture and dependency map

| Responsibility | Source and seam | Observed boundary |
|---|---|---|
| Public values and operations | `include/hail.h`, `include/hail/virtual.h` | Fixed-width neutral records, opaque contexts/streams, PCM frames, explicit directions and policy. No SDK handles or application callbacks. |
| Representation, storage and flow | `src/queue.c`: `hail_format`, rings, `hail_capture`, `hail_render` | Checked geometry, bounded copies, capture drop-newest, render silence, optional reference. No thread/device APIs. |
| Discovery, planning, lifecycle | `src/core.c`: selectors, plans, open, `progress`, close/destroy | Serialized control owns registry, quotas, generations and state transitions. Native operations enter through `hail_backend_ops`. |
| Native ownership and time | `src/native.c`: gates, snapshots, error mailbox, `native_clock`, registry publication | SDK-free per-leg handoff and neutral time facts. The backend catalog is composition, not SDK processing. |
| Native resources and execution | `src/backend_coreaudio.c`, permission helper, WASAPI/PipeWire/ALSA adapters | SDK callbacks/workers, native buffers and format/error translation. Resources are retired before portable storage is reclaimed. |
| Deterministic execution | `src/virtual.c` | External serialized steps and rational synthetic clocks, using the same queue semantics. Its configuration/test state currently occupies generic stream storage (A3). |
| Application processing and service | External clients described in `docs/roadmap.md` | AEC, VAD, resampling, codecs, STT/TTS, HTTP sessions, credentials and conversation scheduling remain outside core. No such implementation was added here. |

Dependencies run from the application through neutral API/control/queues, and
from native adapters into the private handoff and queues. Portable code does not
call SDK APIs. Native resource acquisition and native execution may legitimately
share one adapter; these SDK mechanisms are not arbitrary interchangeable axes.
There is no architectural reason to invent separate callback, worker and device
vtables merely to make the diagram symmetrical.

### Execution traces

**Callback capture:** AUHAL calls `ca_audio`, which validates native frame and
buffer geometry, claims `hail_rt_enter` for capture, obtains PCM through
`AudioUnitRender`, and splits it into application quanta. `hail_native_capture`
normalizes time/epoch facts and `hail_capture` publishes copied PCM and metadata
through `hail_ring_push`. `hail_rt_leave` publishes a bounded counter snapshot.
`ha_stream_read`/`hail_ring_read` copies a slot or partial suffix and advances its
consumer cursor. PipeWire's `stream_process` similarly validates its descriptor,
claims the leg, copies bounded chunks and returns the native buffer lease with
`pw_stream_queue_buffer`. Native buffers never escape as public PCM pointers.

**Worker capture:** `alsa_worker` polls and calls `alsa_process`; under its leg
gate it checks availability, reads at most a quantum and supplies estimated
host time with unknown uncertainty. WASAPI's `wa_stream_worker` dispatches an
audio event to `wa_capture`, checks each acquired packet, splits it into quanta,
publishes neutral time/PCM, and releases the packet through the SDK. A native
event or availability count is a request to service audio, not proof that an
entire application block was transferred.

**Externally driven capture:** `ha_virtual_step` runs on the serialized driver,
validates the requested step, supplies caller PCM or `synthetic` scratch,
advances a rational clock with `hail_clock_info`, then calls the same
`hail_capture`. No thread or callback is invented. Native gate/snapshot ownership
is intentionally unnecessary in this execution model; A2 concerns how that
choice is encoded, not a requirement to make these mechanisms identical.

**Playback and reference:** `ha_stream_write` validates state, generation and
write-role ownership, then enqueues a bounded prefix. `ca_audio`,
`stream_process`, `alsa_process` or `wa_render` select PCM under render ownership
through `hail_native_render`/`hail_render`. Missing PCM becomes silence; reference
records describe that selection. ALSA retains a short native write's pending
suffix independently of the application ring's partial-slot cursor. Application
acceptance, render selection, native submission and audible presentation are
different events. Native reset can invalidate a previously predicted reference
range; a reference queue is not proof of DAC output.

**Duplex interruption:** `ha_stream_flush_playback` closes write admission,
increments playback generation and captures a producer cutoff. `progress`
closes only the render gate and calls backend `flush`. Native adapters stop/reset
render only; completion discards the old partial slot/cutoff, updates render
generation, starts a new render epoch, reopens render ownership where running,
and publishes sticky completion. Capture has its own gate, queue and epoch.
Virtual flush performs equivalent logical discard on the serialized driver.
Control can block in SDK operations, so application capture servicing must remain
independently scheduled; a capture-preserving flush does not promise bounded
control-call latency. Native duplex qualification is still platform-specific.

**Stop, errors and close:** `ha_stream_stop` requests STOPPING; `progress` revokes
native admission, checks backend stop and application roles, then clears queues
and advances generation. Close instead requires `retire`, which now joins the
ALSA worker or observes signaled WASAPI thread termination before reclamation.
An EXIT acknowledgement is not thread termination. SDK callbacks/listeners need
their own retirement barrier even when Hail gates are closed. Application users
must also be quiescent before `ha_stream_close_finish`.

**Failed open:** core owns the failed allocation's cleanup. It tries `retire`,
then the final `stream_free` hook. If retirement is pending or fails, the
unpublished stream remains in the bounded stream table and memory budget with
no public handle. Poll or destroy retries it. This avoids conflating construction
failure, public handle publication and physical resource lifetime. Failed
context initialization still uses the existing conservative quarantine when
native cleanup cannot be proved; it has no published handle for later retries.

**Discovery and permissions:** native notifications publish dirty flags or
bounded removal identities; control refresh builds authoritative registry copies.
`hail_registry_publish` preserves an incarnation only when its native identity
still matches. Default notifications do not retarget pinned streams. Probe/open
permission checks report access facts; CoreAudio's worker caches revocation
without calling consent UI or granting permission. Availability, permission and
successful format negotiation are separate facts; a probe is advisory.

**Timing:** each leg owns its logical position and clock epoch. `native_clock`
tracks device continuity and host correlation, marks estimated/unknown time
honestly, and advances epochs on reset/discontinuity. Partial reads interpolate
the slot's rational time. `generation` instead controls playback content
validity. Equal requested rates, shared physical device names or simultaneous
callbacks do not establish a shared clock; duplex need not use equal formats.

### Findings

| ID | Severity/status | Source | Principle and concrete consequence | Smallest reasonable action |
|---|---|---|---|---|
| A1 | Medium, fixed | [native.c:122](../src/native.c#L122), `hail_rt_collect` | Terminal result and its native diagnostic are one cause. Previously capture could set `HA_DEVICE_LOST`/code 11, then render's permission failure overwrite only the native code with 22. Repeated status collection perpetuated the mixed diagnosis. | Preserve the first control-observed terminal result/payload together. `tests/retirement.c` now checks both legs failing and repeated/closing status reads. |
| A2 | Low, open | [core.c:375](../src/core.c#L375), `ha_stream_open`; `queue.c`, `native.c` | Execution ownership is inferred from platform/backend identity: every backend except virtual is `native`. This currently matches the implemented adapters, but a new serialized externally driven adapter cannot choose direct counters/control ownership without masquerading as virtual or adopting the native handoff. | Before adding another execution model, make ownership mode an explicit private backend property. Keep scheduling native; do not expose a public scheduler or multiply backend classes. Test the same portable behavior with both ownership modes. |
| A3 | Low, open | [hail_internal.h:98](../src/hail_internal.h#L98), [core.c:316](../src/core.c#L316), `virtual.c:synthetic` | Virtual fault controls, drift and rational-clock state are embedded in every stream. Generic planning also reserves a capture scratch buffer used only by synthetic virtual capture. A real capture stream therefore pays a virtual-only allocation cost, up to 65,536 scratch bytes at maximum stereo F32 quantum. | Move virtual-only state and synthetic scratch into the virtual adapter's planned `stream_bytes` when doing the next private layout cleanup. Preserve exact plan/open accounting, freestanding arena support and public ABI. No correctness failure is demonstrated by the current extra storage. |
| A4 | Low, fixed | `docs/architecture.md` lifecycle table; `native.c:hail_rt_enter` | The design table allowed native queue activity in STARTING, while gates admit only RUNNING. Native SDK startup and published Hail running state were described as the same transition. | Document actual admission: application playback priming is allowed in STARTING; native queue servicing waits for RUNNING. A deterministic test checks the gate behavior. |

A2 and A3 are source-confirmed design debt, not evidence that the current
supported backends corrupt audio. Neither warrants a broad refactor during a
safety fix. The C audit's worker-barrier, native-stop and stride fixes address
additional places where cancellation/retirement or descriptor/PCM concepts had
been conflated; their resolution is recorded in the
[C audit](c-audit-2026-11-17.md#resolution-pass-2026-11-17).

### Compatibility and evidence

| Backend/execution | Implemented/buildable | Exercised in this pass | Qualification limit |
|---|---|---|---|
| Virtual, serialized external driver | Yes; hosted and freestanding | macOS/Linux conformance and concurrency; macOS ASan/UBSan/TSan; Linux ASan/UBSan | Deterministic transport evidence, not native device evidence. |
| CoreAudio, SDK callbacks plus permission worker | Yes; static/shared macOS | Device-free tests, static analysis, normal and sanitizer builds | Existing [macOS qualification](coreaudio-qualification.md) remains separate. No physical microphone/output was opened in this pass. |
| ALSA, polling worker | Yes; Linux static/shared | Null PCM, injected drop/join failures, portable tests; ASan/UBSan | No physical ALSA endpoint qualification in this pass. |
| PipeWire, SDK data-loop callbacks | Yes; Linux static/shared | Isolated software loopback, injected malformed strides, portable tests; ASan/UBSan | Software loopback does not prove physical latency or arbitrary device/driver behavior. |
| WASAPI, COM/event worker | Yes; Windows x64 cross-build including new fault tests | Compilation/linking only here | Run `wasapi_retirement` and actual duplex streaming on native Windows. Neither is claimed executed by this pass. |
| Embedded/other execution | No adapter implemented | Freestanding portable compile and virtual tests only | A2 must be addressed when adding another owner model. No DMA/ISR support claim. |
| DSP/speech/network service | Outside Hail by design | Boundary/source review only | Implement/qualify external adapters and service per the roadmap. |

### Contract and next steps

Control owns plans, identity, state and completion; each audio leg owns queue
production/consumption and current timing/counters while admitted. Application
data roles are separate bounded producers/consumers. Native buffers and SDK
resources remain adapter-owned. Snapshots/error mailboxes transfer facts to
control without SDK types. Failed native operations cannot claim successful
interruption; cancellation cannot authorize freeing storage. Preserve one
terminal cause, while generations and clock epochs answer different questions.

Run the new WASAPI fault test on Windows, then the
[native duplex qualification](native-windows-goal.md). Carry A2/A3 as private
cleanup work before another execution adapter, with plan/allocator and shared
conformance tests. Keep resampling/AEC/VAD, endpoint HTTP, model scheduling and
conversation cancellation above the transport API. The existing
[roadmap](roadmap.md) records that cross-repository scope.

The expanded `tools/check_contracts.py` scans portable/public files for SDK
headers/calls and checks exact archive/shared exports. Checks passed on macOS
and Linux, including the 22-symbol backend-free build. This proves the inspected
syntactic boundary, not all semantic separation. Full commands, test counts and
limitations are in the linked C audit resolution. No native SDK scheduling
framework or public API was added.
