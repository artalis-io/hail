# Native backend contracts and qualification

Gate D implements four independent adapters behind the private operation table.
The portable core still includes no OS headers, native clocks, threads or device
manager. No third-party audio wrapper or DSP engine is included. Version 0.1 is
provisional: implementation is complete, platform qualification is not.
See [verification](verification.md), [CoreAudio loopback results](coreaudio-qualification.md)
and the [WASAPI qualification design](wasapi-loopback-design.md) for exact evidence
and remaining tests.

The [Windows qualification harness](wasapi-qualification.md) now implements
explicit-key physical/paired tests, fixture validation and a provisioned workflow.
[Local Windows results](native-windows-results.md) add device-free regression,
static/shared and installed-consumer evidence; native WASAPI streaming remains
unqualified pending actual authorized endpoint/fixture runs.

## Selection and discovery

CoreAudio and WASAPI are enabled by default on their target OS. Linux backends
are opt-in; when both are compiled, AUTO prefers PipeWire, then ALSA. AUTO falls
back only on unsupported/unavailable initialization, never on permission denial,
resource exhaustion or a failed stream. AUTO never selects the virtual backend.
Select an explicit backend when reproducibility matters. There is no migration
between backends or automatic stream recovery.

Enumeration is a copied, bounded snapshot. Control-owner polling reports native
registry/default changes; call refresh to commit a fresh snapshot. Removal can
invalidate pinned streams before refresh. Default selection resolves once at
open and never follows later defaults. Communications-role defaults are currently
unsupported. Permission to enumerate does not imply permission to open/start.

Runtime IDs belong to a context and an observed endpoint incarnation, not an
index. CoreAudio UIDs, WASAPI endpoint IDs and ALSA configuration names are
optional OS-scoped persistent keys, not universal physical-device identities.
PipeWire object serials are runtime identities only. Notification coalescing and
OS identifier reuse prevent a guarantee that every arbitrarily brief unplug is
observable. Device failure always requires closing and explicitly reopening.

## Backend differences

| Adapter | Discovery and execution | Negotiation | Interruption |
| --- | --- | --- | --- |
| [CoreAudio](../src/backend_coreaudio.c) | AudioObject registry; separate AUHAL capture/render units; HAL audio callbacks | Exact device nominal rate required. AUHAL PCM/channel conversion allowed only under exact-application policy. Strict mode requires a verified matching device format. | Stop/reset/restart only the render unit, after callback retirement; capture unit remains active. |
| [Shared WASAPI](../src/backend_wasapi.c) | Dedicated COM context worker; separate event-driven COM stream worker; endpoint notifications | Exact application PCM with Windows conversion explicitly permitted; mix format reported separately. Physical format unknown; strict end-to-end mode unsupported. | Render Stop/Reset/Start on owning worker; capture client remains active. |
| [PipeWire](../src/backend_pipewire.c) | Registry, default metadata, own thread loop; RT_PROCESS data-loop callbacks; explicit sink-monitor capture endpoints | Exact application SPA format; server conversion explicitly reported. Physical device format unknown; strict end-to-end unsupported. | Deactivate/flush/reactivate render stream with server acknowledgement; capture stream remains active. |
| [ALSA](../src/backend_alsa.c) | PCM hints; nonblocking PCM I/O on own pthread; /dev/snd inotify | Exact application parameters. Strict end-to-end limited to HW/null handles with automatic conversions disabled; other plugins have unknown conversion. | Render drop/prepare, with retained partial write discarded; capture handle remains active. |

Linux enumeration intentionally includes default, null, hw and plughw hints,
not every DSP/surround alias. The default/configuration name may route through a
server; Hail cannot promise physical pinning behind an externally reconfigured
plugin. ALSA has no common desktop default-change service: explicit refresh is
needed for configuration-file changes. PipeWire uses the session's routing and
permissions; monitor capture observes a sink, not a microphone. Unavailable
session services produce errors, not a fabricated empty working device.

CoreAudio does not set device-wide rate, create aggregate devices, request
microphone permission dialogs, or change process privacy policy. The host must
supply its microphone usage description and obtain OS authorization. Windows
privacy settings and Linux device/session permissions likewise remain binding.
Probe can activate/configure a native endpoint and is advisory, never a grant.
CoreAudio queries AVFoundation's existing authorization state synchronously at
probe/open/start, where known denied/restricted capture fails explicitly. A lazy
private context worker checks revocation while capture streams exist, waiting
250 ms between SDK queries; poll only consumes the atomic result and publishes
failures. SDK query duration and owner polling add to observation delay; no hard
revocation deadline is promised. AUHAL can otherwise
open/start despite an OS denial. Undetermined is not a permission grant; hosts
must authorize physical capture normally. No authorization query runs on audio
callbacks, and the library never displays a permission request.
Context destruction joins this worker before freeing its synchronization/storage.
No worker runs for enumeration/playback alone, and an existing worker sleeps
without querying while no capture streams remain. Native SDK reads can block;
they must not be placed on every control poll or a PCM producer's hot path.

## Timing and reference

Each leg has its own logical frame timeline and epoch. Raw positions are marked
valid only when native units are sample frames. CoreAudio sample/host timestamps
and WASAPI capture device/QPC timestamps retain their validity conditions. ALSA
delay and PipeWire graph/queue correlations, and WASAPI render predictions, are
estimates marked HA_TIME_ESTIMATED with unknown uncertainty (UINT64_MAX).
Unknown physical latency is never presented as a measured bound.

Raw position discontinuities or exact host-clock reversal create a new epoch.
An estimated host correlation that reverses loses validity and reports a clock
event. Unknown capture gaps also advance the epoch; known lost frames advance
logical position and loss counters. No clock drift compensation or resampling
is implemented in the core. A shared clock is reported only when positively
identified; separate equal-rate endpoints are not thereby synchronized.

Reference PCM records Hail's render selection, including silence, before native
submission. A native write can subsequently fail, or a reset can discard it.
Flush advances the render epoch and playback generation; it preserves capture.
Acknowledgement means the old Hail/native submission has unwound, not that a
remote DAC has stopped audibly. Neither submitted downstream samples nor already
audible samples can be retracted universally. Physical interruption latency has not been measured; software acknowledgement
and loopback delivery measurements are in [qualification](loopback-qualification.md). External AEC must account for missing/estimated timing and resets.

## Memory, concurrency and retirement

All allocated Hail-owned native state and fixed scratch are included in context/stream
plans and the byte budget. Framework, driver, server and thread-stack memory
are outside that budget; native APIs can allocate during setup/control. There
are no Hail heap operations in steady-state processing. Framework internals are
not certified allocation-free or hard real-time. Native servicing validates
plane count, frame count, bytes and stride before portable transfer. Quantum
is at most 8192; larger unsupported native buffers fail explicitly.

One producer owns each audio leg. Single-attempt atomic gates serialize audio
submission against reset/flush/retirement. Two-slot SPSC snapshots transfer
counters/events to control without making audio wait for polling. A separate
atomic error claim publishes a native diagnostic once; notification threads
never write plain control state. Queue/event/snapshot capacities are fixed.
WASAPI removal notification staging has 16 MPSC slots; overflow conservatively
invalidates old endpoints and requests a rescan rather than hiding replacement.

Close revokes admission and proves native retirement before freeing state.
ALSA joins its worker; WASAPI releases clients on their COM owner and joins;
PipeWire removes its listener through the native data-loop synchronization
barrier before destroying streams; CoreAudio stops/uninitializes units and
checks callback retirement. Context listener removal can return PENDING; only
retry context destruction once that closing phase begins. No timeout permits
freeing storage still reachable by native callbacks.

PipeWire's process initialization is once per process and intentionally never
paired with process-global deinitialization, so one Hail context cannot tear
down framework state used by another client. There is no mutable Hail-wide audio
configuration. Applications should treat thread-loop roundtrips and COM/control
operations as potentially blocking; keep them off a UI or language-runtime
execution thread that must stay responsive.

## Native API basis

AUHAL input/output scopes, preallocated input buffers and device binding follow
[Apple TN2091](https://developer.apple.com/library/archive/technotes/tn2091/_index.html).
Endpoint notification storage remains owned until unregister because the native
registration API does not retain it through AddRef/Release, as specified by
[Microsoft](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-immdeviceenumerator-registerendpointnotificationcallback).
ALSA's nonblocking transfers, EAGAIN and xrun state transitions follow its
[PCM documentation](https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html).
PipeWire retirement was checked against the native stream implementation's
listener-removal/data-loop synchronization, not inferred from cancellation:
[stream.c](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.8/src/pipewire/stream.c).
Native SDKs/libraries are system dependencies; no SDK or third-party source is
vendored. ALSA linking carries its LGPL obligations; PipeWire's library is MIT;
platform frameworks retain their platform SDK terms. Consult dependency license
files for the specific distribution when shipping, especially static binaries.
