# Hail backend authoring boundary

The private [backend contract](../src/hail_internal.h) versions with source; it
is not an installed binary plugin ABI. Virtual, CoreAudio, shared WASAPI,
PipeWire and ALSA drivers implement it. The virtual driver serializes processing
on the external control owner; native drivers use the handoff below. Neither
model should be copied into the other without preserving ownership.

The context owns a fixed-capacity neutral device registry, default endpoint IDs,
events, allocator and stream table. Backend discovery supplies copied metadata
and permission facts; selectors resolve independently of enumeration indices.
Probe validates a requested format against a selected endpoint. Open publishes
all negotiated facts and must return failure only after retiring any unpublished
native references. Context/stream plans account for every Hail allocation.

Native execution uses [native.c](../src/native.c) and per-leg admission gates.
Audio owns its leg's queue producer/consumer, frame positions, clock tracker and
current counters while admitted. Control closes that gate before reset, flush
or retirement and accesses final counters only after ownership is acquired.
Two-slot SPSC snapshots transfer cumulative counters and event bits with
release/acquire indices. Full snapshots never stall audio; final collection
preserves totals. An atomic error claim publishes the native payload before a
release error result; control ignores an in-progress sentinel. Notification
threads never mutate control state or call hail_event. Registry changes are
copied to control or staged in bounded mailboxes.

The public data roles remain one reader/writer per queue. The private native
producer is separate from these roles. Native listeners must publish retirement
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
