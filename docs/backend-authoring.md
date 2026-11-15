# Hail backend authoring boundary

The private [backend contract](../src/hail_internal.h) versions with source; it
is not an installed binary plugin ABI. Only the deterministic virtual driver is
implemented. It provides a reference for bounded semantics, not a native-thread
implementation to copy without adaptation.

The context owns a fixed-capacity neutral device registry, default endpoint IDs,
events, allocator and stream table. Backend discovery supplies copied metadata
and permission facts; selectors resolve independently of enumeration indices.
Probe validates a requested format against a selected endpoint. Open publishes
all negotiated facts and must return failure only after retiring any unpublished
native references. Context/stream plans account for every Hail allocation.

The current driver serializes context control, lifecycle progress and audio
processing on the external control owner. Capture/render queues can concurrently
serve their dedicated application roles. In this model queue metadata, counters,
clock construction and event publication belong to the driver owner; they are
not independently safe for arbitrary OS callback threads to mutate.

Before introducing a native RT callback/worker, extend the private seam with
the command/acknowledgement mailboxes and per-leg snapshot transfer described
in the approved architecture. Do not call the current `hail_event`, modify
control status or use plain shared 64-bit clock/generation values from such a
thread. Core public API/ownership semantics stay intact, but this private
execution adapter is a required Gate D task. TSan conformance must cover the
actual native topology, not merely the externally driven virtual topology.

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
native periods and test skips belong in the support evidence. Gate C does not
assert that a virtual-only conformance result validates native audio.
