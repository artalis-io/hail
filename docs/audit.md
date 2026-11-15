# Hail repository and audio implementation audit

Investigation date: 2026-11-15. This is the Gate A deliverable and the evidence
for the [Gate B proposal](architecture.md). Repository statements below refer to
inspected local files, linked relative to this checkout. Upstream audio sources
are linked directly. Backend selection is an engineering inference from those
sources, not a latency or binary-size measurement.

## Repository scope and provenance

| Repository | Inspected revision | Availability |
| --- | --- | --- |
| Keel | `1265b91820ac30b0af9a1f198916dead699735d7` | Local checkout, clean at audit |
| Hull | `760f27a0c9eb7e932adcac3662a85608511aa945` | Local checkout, clean at audit |
| Sail | Unknown | No checkout found under `/home/mark/artalis.io`; `gh api repos/artalis-io/sail` returned HTTP 404. That does not distinguish absent from inaccessible. |
| bitnet.c | `e2fb6b6f2e8c0ee2d00216175e9a4e55ae28f90d` | Local, substantially modified worktree; inspected separately, not asserted to be Sail |
| Hail | Initial empty worktree | No preexisting production API or implementation |

The [bitnet.c positioning note](../../bitnet.c/docs/sail-positioning.md)
recommends a Sail rename and describes a prospective inference sibling. This
does not establish a shipped Sail ABI. A complete Sail architecture and export
audit remains a prerequisite to ABI freeze when that repository is available.
No sibling files were modified and no sibling test suite was run during this audit.

## Keel findings and their implications

| Concern | Inspected evidence | Implication for Hail |
| --- | --- | --- |
| Organization | [Makefile](../../keel/Makefile), [architecture overview](../../keel/docs/architecture/overview.md), `include/keel`, `src/protocols`, `integrations`, `tests`, `fuzz`, `bench` | Separate semantic core, native adapters, optional integrations, and verification. Hail need not copy a protocol hierarchy. |
| Independent axes | [overview](../../keel/docs/architecture/overview.md), [event capability negotiation](../../keel/src/event_caps.h) | Transport semantics, execution engine, and network provider are independent. Hail should separate queue/timing semantics from backend execution without inventing equivalent networking axes. |
| Dependency enforcement | [invariants I6, I7, I10](../../keel/docs/architecture/invariants.md), Makefile `check-tier1-boundary`, `check-sockaddr-neutral` | Make forbidden OS/DSP/runtime dependencies mechanically checkable. |
| Public interface | [stream.h](../../keel/include/keel/stream.h), [stream_detail.h](../../keel/include/keel/stream_detail.h) | Stable function/ownership surface can be separated from private layout. Hail should keep layout opaque entirely. |
| ABI | [compatibility contract](../../keel/docs/contracts/compatibility.md), [version.h](../../keel/include/keel/version.h) | Keel promises source plus static-relink compatibility within a major, explicitly not binary compatibility. It uses append-only zero-default configs, no size/version fields. Hail's requested shared ABI requires different machinery. |
| Errors | [error.h](../../keel/include/keel/error.h) | Keel uses portable diagnostic codes on owners alongside failure returns. Hail should return portable status directly and separate diagnostics from real-time events. |
| Memory | [allocator.h](../../keel/include/keel/allocator.h), [stream_write.c](../../keel/src/stream_write.c) | Allocator injection, validated operation tables, and preallocated bounded admission are useful precedents. Audio overflow cannot backpressure a microphone. |
| Ownership and close | [stream.h](../../keel/include/keel/stream.h), [stream_close.c](../../keel/src/stream_close.c), [async lifecycle](../../keel/docs/contracts/async_lifecycle.md) | Logical cancellation is distinct from physical retirement. Never reclaim storage merely because cancellation was requested. |
| Reentrancy | [stream_close.c](../../keel/src/stream_close.c), [stream_read.c](../../keel/src/stream_read.c) | Inline completion and callback unwinding require explicit guards. Hail reduces public reentrancy by avoiding user audio callbacks. |
| Threading | [thread_pool.c](../../keel/src/thread_pool.c), [event_ctx.h](../../keel/include/keel/event_ctx.h), [agent guide](../../keel/AGENTS.md) | Workers and loop ownership are explicit; networking's event-loop serialization cannot be assumed on OS audio callbacks. |
| Clock | [clock.h](../../keel/include/keel/clock.h) | Keel's monotonic milliseconds suit deadlines. Audio needs frame positions, device domains, correlations, and uncertainty as well. |
| Build and features | [Makefile](../../keel/Makefile), [toolchain.mk](../../keel/mk/toolchain.mk), [integrations](../../keel/integrations/README.md) | Actual build is Make, with normalized compiler intent and platform/backend source selection, not CMake. Hail uses CMake as requested, preserving feature exclusion and downstream flag control. |
| Embedded and UEFI | [freestanding.h](../../keel/include/keel/freestanding.h), [platform support](../../keel/docs/operations/platform_support.md), `integrations/platform` | Keel exposes a restricted freestanding header subset and BYO adapters. Hail should start with a clean portable core, and distinguish compile gates from functional firmware support. |
| Verification | [CI](../../keel/.github/workflows/ci.yml), [testing](../../keel/docs/operations/testing.md), [watcher ABA regression](../../keel/tests/test_watcher_aba.c), [single-shot regression](../../keel/tests/test_stream_single_shot.c) | Scripted backends and deterministic allocator reuse test lifetime failures. CI contains native Windows/macOS/Linux lanes, sanitizers, fuzzing and seam checks; those are workflow definitions, not results rerun here. |
| Security and dependencies | [SECURITY.md](../../keel/SECURITY.md), [compatibility](../../keel/docs/contracts/compatibility.md), `vendor`, `integrations` | Keep integration types out of public headers. Keel's static/no-runtime-plugin policy is evidence about Keel, not a requirement to impose on Hail's optional shared library. |

The overview contains older HTTP/readiness sections below its current three-axis
introduction. The current invariants and actual source seams govern this audit;
the older io_uring poll-add description is not used to characterize the current
completion backend.

## Hull findings and adapter requirements

Hull's [runtime interface](../../hull/include/hull/runtime.h) defines a
factory-selected Lua/QuickJS runtime and init/destroy hooks. The
[QuickJS module dispatcher](../../hull/src/hull/runtime/js/modules.c) registers
`hull:*` modules, including optional feature seams. Its
[runtime implementation](../../hull/src/hull/runtime/js/runtime.c) sets a heap
limit and interrupt handler in `hl_js_init`, and cancels tracked timers and
releases module state in `hl_js_free`. Hail must be initialized and retired by an
adapter before runtime-owned memory is released; Hail must not store JS values.

The [async backend interface](../../hull/include/hull/shared/async_backend.h)
separates event loops, monotonic time, timers, watchers, pools and operation
completion. It assigns resume/deadline callbacks to the event-loop thread and
worker work to pool threads. The
[shared continuation contract](../../hull/include/hull/shared/async.h) distinguishes
resume, cancel, and destruction. An audio adapter must marshal data/events onto
that execution context; it cannot call QuickJS from an audio thread. Timers or
bounded worker polling are sufficient initially; Hail should not expose native
file descriptors merely to fit Hull's watcher API.

Hull has a [tracking allocator](../../hull/include/hull/utils/alloc.h) with byte
accounting, an optional cap, and a Keel allocator adapter. Its
[filesystem capability implementation](../../hull/src/hull/cap/fs.c) demonstrates
capability checks below bindings. Audio capability policy, device allowlists,
quotas, and cancellation scopes should similarly belong in Hull, outside Hail.
OS microphone permissions still apply independently.

The [Makefile](../../hull/Makefile) implements optional feature archives,
runtime-specific bridges, platform archives and `HL_KEEL_FEATURE` composition.
The [Keel feature document](../../hull/docs/keel_feature.md) describes the
Keel-free composable base. Proposed future `libhull_feature-audio.a` and
runtime bridges should follow that packaging direction, with Hail linked only
when selected. Those audio archives do not exist today.

The [BuildContext design](../../hull/docs/buildcontext_design.md) explicitly says
**DESIGN-ONLY. NOTHING IMPLEMENTED.** It specifies host-owned action transactions,
declared inputs, staged outputs and tool authority. Treat these as future build
integration expectations, not a currently usable SDK. Hail's standalone CMake
install must be usable without that pipeline. No Hull integration is authorized
in the current gates.

## Available inference sibling evidence

In bitnet.c, [backend_model.h](../../bitnet.c/include/backend_model.h) separates
immutable source data from mutable bounded caches, and
[backend_session.h](../../bitnet.c/include/backend_session.h) documents caller
serialization, GPU resource ownership, epochs and teardown order. These are
useful local examples of explicit budgets and borrowed-view validity. Actual
identifiers there are `Bn*`, `bn_*`, and `BN_*`; the positioning note's proposed
Sail name is not evidence for `sl_` or any other future ABI. Hail should share
contract discipline, not inference types, sessions, caches or GPU dependencies.

## Namespace evidence and freeze conditions

[Raw evidence](evidence/namespace.json) records revisions, worktree status counts/hashes,
artifact hashes, every observed global definition and all `ha_`/`HA_` token
matches in tracked C/C++ sources and headers. Reproduce with:

```sh
python3 tools/audit_namespace.py ../keel ../hull ../bitnet.c > /tmp/hail-namespace.json
```

| Scope | Observed result | Limit |
| --- | --- | --- |
| Keel tracked source/header scan | 558 files; no `ha_`/`HA_` tokens | Includes tracked vendor files; excludes untracked files |
| Keel existing `libkeel.a` | 764 distinct global definitions; 642 `kl_`, 121 `llhttp_`; no `ha_`/`HA_` | Existing archive, not freshly rebuilt or checked against HEAD |
| Hull tracked source/header scan | 1040 files; no `ha_`/`HA_` tokens; inspected public declarations use `hl_`/`Hl*` | No Hull application/library artifact available; actual binary exports unverified |
| bitnet.c tracked source/header scan | 375 files; no `ha_`/`HA_` tokens | Dirty checkout; many untracked files outside this scan |
| bitnet.c existing executable | 2957 global definitions, 2701 `bn_`; no `ha_`/`HA_` | Executable symbol table, not a library export table; build provenance unverified |
| Sail | Unavailable | No binary or source namespace conclusion |

These checks support provisionally using `ha_`/`HA_`; they do **not** complete
the collision acceptance criterion. Before ABI 1 freeze: obtain Sail, rebuild
the relevant sibling libraries in isolated writable builds, inventory static
global definitions and shared dynamic export tables for each platform and
feature combination, and intersect full names with Hail's exact export list.
Also scan installed public macros/types and preprocess combined headers in C
and C++. Prefix differences alone are insufficient, as Keel's vendor globals
already demonstrate. Hail internals should use private `hail_*` names with hidden
visibility, not private-looking `ha_*` exports. Static archives require a global
symbol check too; ELF hidden visibility alone does not remove static-link name
collisions.

## Existing audio implementations

| Candidate | Evidence and tradeoffs | Hail fit |
| --- | --- | --- |
| miniaudio | C low-level duplex/device API; single-file integration, configurable conversion, allocation hooks and backend switches. Transparent structs and no inter-release ABI guarantee. Public-domain or MIT-0 choice. [Manual](https://miniaud.io/docs/manual/index.html), [source](https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h). | Strongest fallback for reducing backend engineering. Hail would still own queues, lifetimes and ABI; native clock/flush parity needs a prototype rather than assuming the wrapper supplies it. |
| PortAudio | C API with explicit initialize/terminate, device/host-API indices, callback or blocking streams, full-duplex; no general sample-rate converter. [Overview](https://portaudio.com/docs/v19-doxydocs/api_overview.html). MIT-style license; callback carries ADC/DAC/current timestamps and xrun flags, and prohibits unpredictable work. [Header](https://raw.githubusercontent.com/PortAudio/portaudio/master/include/portaudio.h). | Useful reference and candidate adapter. Process initialization and public timing/device abstractions would require adaptation; abort is a stream operation, not proof of playback-only flush preserving capture. |
| RtAudio | C++ classes with native backends, stream options, arbitrary device IDs, callback overflow/underflow status and stream-relative seconds. MIT-style license. [Header](https://raw.githubusercontent.com/thestk/rtaudio/master/RtAudio.h), [license](https://raw.githubusercontent.com/thestk/rtaudio/master/LICENSE), [repository](https://github.com/thestk/rtaudio). | Could be hidden behind C ABI but introduces a C++ toolchain/runtime into an otherwise C implementation. Callback stream-time alone is insufficient for Hail's proposed clock-domain metadata. |
| WebRTC Audio Processing | Voice DSP, primary/reverse streams and a C++ API with WebRTC/Abseil dependencies; BSD-style terms and PATENTS notice. Enabling components allocates. [AudioProcessing interface](https://webrtc.googlesource.com/src/+/refs/heads/main/api/audio/audio_processing.h). | A consumer of timestamped capture and render reference, not a device-I/O backend. Keep out of core. |
| Native APIs | Direct endpoint identity, native position/timestamps, format negotiation, invalidation and reset primitives. Evidence below. | More backend code and maintenance responsibility, but a direct path to proving Hail's narrower semantics. |

All three I/O candidates can be compiled into libraries; that does not establish
the final linked dependency closure, footprint or performance. No candidate
latency, overhead, binary size or callback allocation benchmark was run here.
The maintenance tradeoff is fewer owned backend lines versus an upstream update
and integration audit obligation. A license manifest and dependency closure
review remain necessary for the exact selected revisions and enabled backends.

## Native API evidence and proposed backend strategy

**macOS:** CoreAudio HAL exposes devices; AUHAL can capture and can enable both
directions for one suitable device. Separate input/output devices may require
separate instances; creating an aggregate device is not a neutral policy.
[Apple hardware and AUHAL guide](https://developer.apple.com/library/archive/documentation/MusicAudio/Conceptual/CoreAudioOverview/ARoadmaptoCommonTasks/ARoadmaptoCommonTasks.html).
Apple requires a microphone purpose string for APIs accessing the microphone.
[NSMicrophoneUsageDescription](https://developer.apple.com/documentation/bundleresources/information-property-list/nsmicrophoneusagedescription.md).
Propose device-pinned HAL/AUHAL operation with property listeners, explicit
permission failure, and separately controlled playback. Exact IOProc versus
AUHAL implementation is a Gate D spike; shared-clock reporting requires native
evidence, not matching device display names.

**Windows:** WASAPI shared mode has event-driven initialization and
IAudioClient3 period negotiation; invalidated endpoints require recovery.
[Initialize](https://learn.microsoft.com/windows/win32/api/audioclient/nf-audioclient-iaudioclient-initialize),
[WASAPI overview](https://learn.microsoft.com/en-us/windows/win32/coreaudio/wasapi),
[invalid-device recovery](https://learn.microsoft.com/en-us/windows/win32/coreaudio/recovering-from-an-invalid-device-error).
IAudioClock position units must be interpreted using its frequency; correlated
QPC is in 100 ns units and Reset restarts position at zero.
[GetPosition](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclock-getposition).
Propose backend-owned COM-initialized control/worker threads, separate capture
and render clients, endpoint notification staging, and render Stop/Reset for
stronger flush. COM ownership and interface marshaling need native tests.

**Linux ALSA:** PCM has explicit states, drop versus drain, prepare/recovery,
delay and timestamp queries. mmap still requires ownership/commit discipline.
[PCM guide](https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html),
[PCM API](https://www.alsa-project.org/alsa-doc/alsa-lib/group___p_c_m.html).
Propose separate nonblocking capture/playback PCMs and backend worker processing;
never claim that an ALSA plugin is direct hardware. alsa-lib's COPYING is
LGPL-2.1; static redistribution requires an appropriate compliance plan.
[License](https://raw.githubusercontent.com/alsa-project/alsa-lib/master/COPYING).

**Linux PipeWire:** streams negotiate formats/buffers through a server adapter
that can convert; realtime process callbacks prohibit blocking work.
[Streams guide](https://docs.pipewire.org/page_streams.html).
Its clock snapshot distinguishes graph ticks/rate, delay, queue and resampler
buffering. [pw_time](https://docs.pipewire.org/structpw__time.html).
Its stream interface provides RT process selection and flush/drain primitives.
[Stream API](https://docs.pipewire.org/group__pw__stream.html).
PipeWire's top-level license is MIT-style.
[COPYING](https://raw.githubusercontent.com/PipeWire/pipewire/master/COPYING).
Propose native PipeWire for desktop session routing and ALSA as an independent
option for direct/embedded-like Linux use. Server session permissions and routing
must remain observable. Neither backend can promise a physical hardware format
when a server/plugin hides it.

**Decision proposed for review:** own native CoreAudio, shared WASAPI, PipeWire
and ALSA adapters behind a Hail-private seam. Build the virtual backend first.
Default Linux AUTO tries compiled PipeWire then ALSA only for backend absence,
never to escape permission denial or selected-device failure. Explicit backend
selection disables fallback. No runtime download or plugin registry.

This choice follows the required endpoint/clock/flush control, C core and
deterministic testing seam; it is not a claim that native code is faster. Cost:
four native implementations and platform CI ownership. If Gate D cannot prove
the contracts economically, review a miniaudio-backed implementation using the
same conformance suite before broadening the API.

## Open evidence requirements

Sail source and actual Hull exports remain unavailable. Native permission,
clock, callback retirement and playback-reset behavior need target-platform
spikes and hardware validation. Minimum OS/SDK/dependency versions, licensing of
the Hail project itself, and a measured dependency/size comparison are still
unresolved. None prevents reviewing the portable contract, but none should be
silently marked complete when freezing ABI or claiming production support.
