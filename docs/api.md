# Hail portable API and ownership

The current API is in [hail.h](../include/hail.h); deterministic driving is in
[hail/virtual.h](../include/hail/virtual.h). This is version 0.1, with ABI major
zero. Layouts and signatures are provisional until native-platform validation
and the remaining sibling namespace audit. The [approved architecture](architecture.md)
defines the intended long-term contracts; this reference states what is
implemented now.

## Initialization and ownership

Initialize every top-level input/output record with `HA_INIT(type)`, and PCM
with `HA_FORMAT_INIT(rate, channels, sample_format)`. Initialize nested selector
records too; the default selector names the current default at open time.
Disabled-leg formats/selectors are ignored. An entirely zero allocator selects
hosted allocation unless `HA_FREESTANDING` excludes it. An explicit allocator
must have a valid header and both callbacks. No callback may reenter Hail.

`HA_INIT` uses C99 designated initialization in C and a C++11 value expression
in C++. Hail itself requires C11. All output records preserve their supplied
size/version prefix. Short records and unknown versions fail. A larger version-1
record retains its unknown tail unchanged. All currently published fields are
required; future optional appended fields must define an older accepted prefix.

Pointers to configs, selectors, PCM and output records are borrowed only during
the call. None are retained. The allocator callback table is copied; its user
state must outlive successful context destruction. A construction diagnostic
pointer is an optional borrowed output and must itself have a valid header.
Inputs and outputs must not overlap, and buffers must hold their declared
capacity in the relevant negotiated format. A frame is one sample per channel.

The context owns streams and registry resources, but context destruction does
not recursively destroy streams. `ha_context_destroy` returns BUSY while any
stream exists. With no streams, native notification retirement may return PENDING;
retain the pointer/allocator and retry only destruction until success. Stream destruction uses close_begin, control progress, application
quiescence and close_finish. A successful destroy NULLs the caller's pointer
slot; destroying that same NULL slot again succeeds. Never use another stale
alias. No library can diagnose arbitrary calls through freed opaque pointers.

`ha_result_string` returns an immutable static string, valid until library
unload. It is never freed or mutated. No global last-error state exists.

## Function contracts

| Function | Behavior and failure conditions |
| --- | --- |
| `ha_get_version` | Copies linked version, unstable ABI zero and compiled feature bits. |
| `ha_context_plan` | Validates limits/allocator and returns exact Hail allocation bytes/alignment for context plus registry, stream table and events. Does not allocate or guarantee backend availability. |
| `ha_context_create` | One allocation with explicit backend selection. Selects a compiled native or virtual backend; AUTO tries native availability only, never virtual. Publishes no handle on failure. |
| `ha_context_refresh` | Refreshes the bounded native registry and defaults; virtual registry is already authoritative. It does not grant permission. |
| `ha_context_enumerate` | Copies all current device records into initialized caller records. Insufficient capacity returns TOO_SMALL and required count without partial output. |
| `ha_context_get_status` | Copies allocated bytes, budget, owned stream count (including failed opens awaiting retirement), backend/features and cumulative lost events. |
| `ha_context_poll` | Performs at most one control progress operation per live stream, then copies up to event capacity. Zero output capacity progresses operations without consuming events. Capacity is at most HA_MAX_EVENTS. |
| `ha_device_probe` | Checks selector/direction/permission/PCM and returns advisory application/device format facts. Direction is explicit so a default selector is unambiguous. Open rechecks. |
| `ha_stream_plan` | Validates selectors/PCM/queue geometry and reports the exact stream allocation including queues and scratch. No allocation/reservation; quota can change before open. |
| `ha_stream_open` | Checks quota, allocates one complete stream, and publishes STOPPED on success. Failure output is NULL; optional diagnostic reports construction result. Pending native cleanup retains quota/storage until poll or destroy proves retirement. |
| `ha_stream_get_info` | Copies immutable negotiated formats, resolved endpoint IDs, clock relationships, domains, capacities, bytes and features. Negotiated device format facts are separate per leg. |
| `ha_stream_get_status` | Copies lifecycle, terminal result, generation, last flush result/completion generation, epochs and cumulative losses. Control-owner only. |
| `ha_stream_start` | STOPPED to STARTING; PENDING until poll completes. Flush in progress returns BUSY. Start failure becomes FAILED. New epochs/positions begin; pre-start playback priming remains queued. |
| `ha_stream_stop` | STARTING/RUNNING to STOPPING; cancels pending flush. Completion waits for backend and application role gates, empties queues and changes generation. STOPPED stop is an idempotent no-op, preserving priming. |
| `ha_stream_read` | Copies up to one capture slot, with a partial slot retained for the next read. Empty returns WOULD_BLOCK. Active while STARTING/RUNNING and while STOPPING admits reads. |
| `ha_stream_write` | Copies a prefix into whole available slots, reports accepted frame count. Legal while STOPPED/STARTING/RUNNING. Generation mismatch returns STALE_GENERATION. Full returns WOULD_BLOCK; occupied/revoked role gate returns BUSY. |
| `ha_stream_read_reference` | Same read semantics on optional render-reference queue; disabled queue returns UNSUPPORTED. |
| `ha_stream_flush_playback` | Attempts write admission revocation, then advances generation and returns PENDING. BUSY makes no generation change. Completion is sticky in status and does not depend on receiving an event. |
| `ha_stream_close_begin` | Revokes new I/O and requests backend retirement; repeated calls while CLOSING remain PENDING. Already admitted calls may finish. |
| `ha_stream_close_finish` | Requires application quiescence and prior close_begin. PENDING retains all storage until backend retirement and role gates are closed. Success frees once and NULLs the caller's slot. |
| `ha_context_destroy` | Requires no live public streams and no concurrent API users; retries failed-open cleanup and native listener retirement, returning PENDING while storage remains owned; success frees storage and NULLs the caller's slot. |

Audio transfers accept at most HA_MAX_QUANTUM frames per call, regardless of
queue capacity. Larger requests must be chunked. `quantum_frames` bounds each
backend processing step, while a write may span several smaller slots.
`accepted`/`frames` is zero on transfer failure. A partial write returns OK with
the accepted prefix; the caller must retry its remainder. Zero-frame transfer
permits NULL PCM and returns OK in a legal state, but mandatory output pointers
and record headers remain required. A revoking lifecycle operation can leave
gates closed, so BUSY is also possible during/after revocation; inspect control
status rather than indefinitely retrying a failed/closing stream.

Runtime device IDs are context-scoped incarnations. A persistent-key selector
copies the OS-scoped key from enumeration; keys may be absent and do not promise
cross-host physical identity. Default endpoints are pinned at open. A device
notification requests explicit refresh; active lost streams require close/reopen.

## Formats and memory

Supported application formats are native-endian S16 and IEEE F32, interleaved
mono or front-left/front-right stereo, with integer rates 8000..192000 Hz.
Virtual endpoints support every such request exactly. The portable core performs
no resampling, numerical conversion, mixing, clipping or dither. Native/backend
conversion requires explicit exact-application policy and is reported; strict
end-to-end requests fail when they cannot be verified. Backend/engine and physical
formats have independent validity fields; planar storage is reporting-only. Applications provide finite
F32 values in [-1,1]. Exact application and exact end-to-end negotiation have
identical outcomes on the virtual backend because it performs no conversion.
Unimplemented storage/channel layouts and formats fail explicitly.

Slot counts are powers of two from 1 to 256, default 8 for enabled legs.
Quantum is 1..8192 frames, default 256. Reference is disabled unless its slot
count is nonzero. Registry is at most 64 endpoints, streams at most 16, event
capacity at most 1024 (default 64). Context creation defaults to a 64 MiB total
Hail byte budget. Virtual context creation needs room for its two initial
endpoints; a registry capacity of one returns RESOURCE_EXHAUSTED.

Resource plans expose exact requested bytes/alignment, with one context and
one allocation per stream. The budget includes alignment padding, registry,
queue metadata and preallocated native/synthetic scratch. Allocator-private overhead and
native/server/OS memory are excluded. All Hail allocation/free occurs on
construction/destruction control paths; stepping, reading, writing, flushing,
starting, stopping and event processing make no Hail allocations. Native SDK
control calls may allocate internally. The example uses a
caller-owned arena and also runs with the hosted allocator excluded.

Capture drops newest incoming data when full. The acquisition position advances
for all known frames, and later blocks carry cumulative dropped frames and gap
flags. Playback fills missing frames with silence and counts underruns. A block
SILENCE flag means it contains some silence, not necessarily that every sample
is zero. Optional reference copies selected render PCM including silence;
reference overflow is counted independently and never delays rendering.

## Timing and interruption

Blocks contain first logical frame, clock domain/epoch, optional raw position,
host monotonic nanoseconds, uncertainty, cumulative loss and playback generation.
Zero is a valid timestamp. Validity flags govern every optional correlation.
Frame-time numerator/denominator and remainder support exact partial reads
under rational drift. Epoch changes flag discontinuity; positions and valid
timestamps are ordered only within a continuous epoch/domain. Independent
virtual capture and playback clocks never imply synchronization.

Software flush discards queued Hail playback before completion, leaves capture
running, and blocks writers until acknowledgement is observed by poll. Stale
writers must retain their original generation and fail; relabeling old work
would undo the application's interruption policy. Native-required flush is
supported by the virtual backend, changes only the playback clock epoch, and
can inject delayed/failing completion. The current virtual device has no hidden
native pipeline: inspection returns selected PCM immediately, not a modeled
audible hardware tail. Physical interruption latency is not measured.

Render reference is a selection/submission reference, not proof of DAC
presentation. Software flush cannot retract samples already submitted to real
native/hardware queues, and audible samples cannot be retracted at all. Native reset discards what the backend can retract, advances the render epoch,
and completes only after old native submission has unwound. See the
[native contracts](native-backends.md) for platform differences and test limits.

## Threads and execution contexts

One externally serialized control owner handles every context operation,
stream lifecycle/status/info operation and virtual operation. The virtual
driver shares this role: step and poll must never overlap. It may concurrently
serve one application capture reader, one application playback writer, and one
application reference reader per stream. Role migration requires quiescence.
Multiple callers of one role need external serialization. Independent contexts
can have independent control owners. Native audio execution has one independent producer per leg and never calls
user code. Native counters/errors transfer through bounded release/acquire
snapshots; only control mutates lifecycle/status/events. No user audio callback
is invoked. Control operations may block inside native SDK calls.

Ring indices publish complete data/metadata with release/acquire ordering.
Producer and consumer alone advance their own indices; a producer never moves
a reader's cursor to implement oldest-drop. Fixed 32-bit modular counters are
safe because capacity is under half their range. There is no dynamic queue-node
reclamation or pointer ABA. Single-attempt admission gates never spin. Required
32-bit C atomics must be always lock-free, checked at compile time. No formal
wait-free claim is made for arbitrary platform/runtime implementations.

Stop completion closes all role gates before resetting queues. A role already
inside a call may delay completion. Close_begin can overlap valid application
I/O, but final close_finish cannot: stop/join every caller and prevent new calls
through every alias first. Pending events contain copied IDs, never retained
stream pointers. Failed streams require close/reopen; no recovery retries occur.

## Virtual driver controls

| Function | Contract |
| --- | --- |
| `ha_virtual_configure` | Sets fault flags and poll-count delays. Clock origin/drift can change only while STOPPED; current settings must be preserved when changing faults while running/closing. Drift is bounded to ±1000 ppm. |
| `ha_virtual_step` | Supplies up to one quantum per enabled leg. Capture input is borrowed for this call or synthesized from frame/channel position. Playback is copied into caller inspection storage. Invalid geometry/short playback capacity makes no step. Arithmetic exhaustion fails terminally. |
| `ha_virtual_device_set` | Disconnects, reconnects, or changes permission of the current default endpoint for one direction. Disconnect/revocation fails pinned streams. Reconnection issues a new ID. |
| `ha_virtual_device_add` | Adds another endpoint within registry capacity, without changing the default. |
| `ha_virtual_default_set` | Changes the logical default to an existing matching endpoint. Active streams remain pinned; only future opens resolve the new default. |

Each step's `advance_ns` advances the virtual scheduling clock. Device clocks
advance independently by selected frames divided by their configured effective
rate. The scheduling clock is used as the next origin when injecting an epoch
reset; it does not force clocks with different drift into lockstep. Unknown-gap
injection creates a new capture epoch without inventing a loss count.

FAIL_START, FAIL_STOP, FAIL_FLUSH and PERMISSION_DENIED inject terminal failures.
HOLD_RETIREMENT retains closing storage until explicitly cleared. Delay counts
mean exactly that many poll attempts return pending before the next completes.
There is no time-based timeout or nondeterministic sleep in the driver.
