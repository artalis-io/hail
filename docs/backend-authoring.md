# Hail backend authoring boundary

The private [backend contract](../src/hail_internal.h) versions with source; it
is not an installed binary plugin ABI. Virtual, CoreAudio, shared WASAPI,
PipeWire and ALSA drivers implement it. Backend `execution` explicitly selects
`HAIL_EXEC_SERIALIZED` (audio processing shares the control owner) or
`HAIL_EXEC_CONCURRENT` (per-leg gates/snapshots transfer ownership). Identity and
capability bits do not select that model. Virtual currently uses serialized
ownership; the native adapters currently use concurrent ownership. A new adapter
can choose either without adopting another adapter's identity or scheduling API.

Adapter-specific state belongs in `backend_state`, whose fixed prefix is
`stream_bytes`. An optional pure `stream_extra_bytes` planning hook adds private
space derived from validated stream formats/geometry; core checks the sum and
alignment and includes it in the single stream allocation. The hook performs no
allocation, I/O or mutation and must not depend on the not-yet-computed final
allocation size. Open lays out that same private region. Virtual clocks, fault
controls, progress delays and synthetic capture scratch live only in the virtual
adapter; generic streams and native capture plans contain none of that storage.

The context owns a fixed-capacity neutral device registry, default endpoint IDs,
events, allocator and stream table. Backend discovery supplies copied metadata
and permission facts; selectors resolve independently of enumeration indices.
Probe validates a requested format against a selected endpoint. Open publishes
all negotiated facts. On failure, core calls retire before stream_free and
reclamation. If retirement fails or remains pending, the unpublished stream
stays in the bounded stream table and memory budget; control poll or context
destroy retries retirement. Open must leave partially acquired resources in a
state that retire can safely inspect. Stream_free only performs infallible
cleanup after retirement proof; workers must be joined by retire, not by that
void cleanup hook. Context/stream plans account for every Hail allocation.

Concurrent execution uses [native.c](../src/native.c) and per-leg admission gates.
Audio owns its leg's queue producer/consumer, frame positions, clock tracker and
current counters while admitted. Control closes that gate before reset, flush
or retirement and accesses final counters only after ownership is acquired.
Two-slot SPSC snapshots transfer cumulative counters and event bits with
release/acquire indices. Full snapshots never stall audio; final collection
preserves totals. An atomic error claim publishes the native payload before a
release error result; control ignores an in-progress sentinel. Notification
threads never mutate control state or call hail_event. Registry changes are
copied to control or staged in bounded mailboxes.

The public data roles remain one reader/writer per queue. Backend servicing is
separate from those application roles. Native listeners must publish retirement
through their SDK barrier and an acquire observation before storage reclamation.
Tests exercise concurrent producer threads, final collection, flush, removal,
close and context-destruction retry under TSan. Native callbacks must not take
control locks; backend control methods may use SDK locks/roundtrips and block.

Each leg has exactly one queue producer and consumer. Producer writes PCM plus
metadata before release-publishing its index; consumer acquire-loads before
reading, then release-publishes consumption. Audio processing validates maximum
quantum, frame/byte products and native strides before access. It never allocates,
waits for a queue, logs, calls application code or destroys resources. Native
buffer leases end before callback return unless a documented API explicitly
extends them. Portable copy-based delivery makes no zero-copy promise.

Stop and close revoke admission, then wait for proven native retirement and
application gate ownership before reset/free. Virtual HOLD_RETIREMENT demonstrates
that cancellation is not retirement. Native callbacks/listeners and submitted
operations may complete late or inline; the driver must retain storage until
all have unwound. A guessed timeout cannot authorize destruction. Backend stop,
flush and retire progress methods must be safe to retry after pending results.

Flush uses generation plus a producer cutoff. The playback consumer alone
discards its partial slot and advances to that cutoff. Completion follows old
submission unwinding; optional native reset is render-only and must leave
capture live. Reject unsupported stronger flush before changing admission or
generation. Native failure cannot report successful interruption. Reference PCM
describes submission selection; native reset can invalidate predicted playback.

Clock metadata must state domain, epoch, validity and uncertainty. Never label
callback time as capture/presentation time without a native correlation. A
changed/reset clock creates an epoch; independent endpoints are not synchronized
merely because requested rates match. Native/server format conversion is
explicitly permitted or rejected at open; unknown physical format remains
unknown. No DSP implementation type enters portable headers.

Native platform work must also provide permission, enumeration/default-change,
loss/replacement, format, partial-start rollback and callback retirement tests,
using the same portable failure/state contracts. Hardware, driver versions,
native periods and test skips belong in the support evidence. A virtual-only conformance result does not validate native audio. Current
platform evidence and open qualification items are in [verification](verification.md).
