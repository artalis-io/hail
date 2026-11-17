---
name: axis-audit
description: Audit Hail's audio architecture for separation of PCM transport, execution, platform resources, timing, lifecycle, discovery, and external speech or DSP policy. Trace capture/playback and report concrete cross-axis coupling.
---

# Audio architecture axis audit

Adapted from Keel's `.claude/skills/axis-audit/SKILL.md` at
`8f04a7b150fd8a619d76ef8085efbf380f7e1014`. The inherited principle is semantic
consistency above private seams while preserving honest native mechanisms below
them. Audio callbacks and workers are not networking readiness/completion APIs.

Target: `$ARGUMENTS`, default source, public/private headers, tests, build and
architecture docs. Write `docs/axis-audit.md`, prepending dated passes if it
already exists. Analysis comes first; make only authorized, focused fixes whose
contract is clear. Do not turn an audit into a speculative framework rewrite.

## Map the responsibilities

Start with `docs/architecture.md` (independent axes), `docs/api.md`,
`docs/backend-authoring.md`, `docs/verification.md` and `docs/roadmap.md`.
Verify this map against source rather than treating it as proof:

| Responsibility | Current seam | Must not absorb |
|---|---|---|
| Representation/transport | `hail.h`, `queue.c`, bounded frame copies and metadata | OS buffers, numerical DSP, device scheduling, networking |
| Discovery/identity | Context registry, selectors, runtime incarnations and default notifications | Silent migration, permission grants, guaranteed future access |
| Negotiation/capabilities | Probe/open and per-leg format/conversion facts | Hail resampling, guessed physical facts, capabilities as permission |
| Direction/flow | Independent capture and render legs, optional reference, separate queues | Mandatory equal rates/periods/clocks; render interruption stopping capture |
| Execution/platform | `hail_backend_ops`, `native.c` handoff, `backend_*.c`, `virtual.c` | SDK types in public/portable layers; backend identity defining unrelated semantics |
| Lifecycle/memory | Serialized control, role gates, retirement proof, allocator and quotas | Cancellation as reclamation, unaccounted failed-open state, OS allocation guarantees |
| Timing | Host/device time, domain, epoch, uncertainty, logical frame position | Playback generation, wall clock, synchronization/drift correction |
| Processing/application | External Hull/DSP/speech adapters and herding-sparks service | VAD/AEC/STT/TTS, codecs, HTTP, credentials or conversation policy in Hail core |

Axes are responsibilities, not a demand for a vtable or independently selectable
implementation for every row. Native resource and scheduling APIs may be tightly
integrated inside one adapter. Flag upward semantic coupling, not the mere
presence of both mechanisms in the same translation unit.

## Trace before judging

Use actual function/type names and show ownership at each boundary:

1. Capture through one callback backend (CoreAudio/PipeWire), one worker backend
   (ALSA/WASAPI), and externally driven virtual processing: acquire native buffer,
   validate geometry/time, enter leg ownership, publish PCM/metadata, release
   native lease, read a partial application slot.
2. Playback: application generation check and bounded write, native demand and
   partial submission, silence/underrun, optional reference. Separate queue
   acceptance, native submission and audible presentation.
3. Duplex interruption: generation change, write revocation/cutoff, render
   barrier/reset, acknowledgement; trace capture progress independently.
4. Lifecycle: partial start, stop, terminal error, close while callbacks/workers
   remain, failed open/init, retry and final reclamation. Describe SDK retirement
   proof separately from Hail gates and application quiescence.
5. Discovery: default change versus pinned endpoint removal/replacement;
   permissions at probe/open and later revocation; registry notifications versus
   authoritative refresh.
6. Timing: independent rates/periods, discontinuity, unknown gap, estimated versus
   valid correlation, clock epoch versus playback generation.

## Evidence and judgment

Mechanically inspect portable/public files for SDK includes/symbols, system
error constants, DSP/network/runtime dependencies and platform conditionals.
Inspect `tools/check_contracts.py`; run its archive/shared checks where supported.
A clean scan is evidence of a syntactic boundary, not proof of semantic
independence. Read branches using backend IDs, execution ownership, directions, capabilities,
generation and epoch for hidden cross-axis assumptions.

Use portable conformance, concurrency, native handoff, failed-open retirement
and SDK fault tests to test distinct responsibilities. Run relevant sanitizers
without opening devices. Native loopback/physical qualification is a separate
explicit step; stop devices when testing ends. Do not infer Windows/Linux runtime
coverage from a macOS run or a cross-compile. Do not infer support from code alone.

Report each concrete mixing of concepts with severity, files/symbols, violated
principle, failure/change scenario, evidence status and smallest reasonable fix.
Distinguish deliberate bounded implementation tradeoffs from correctness bugs.
Examples to examine, not presume: virtual-only state embedded in a generic
stream; execution mode keyed to backend identity; lifecycle duplicating SDK
state; one feature bit standing for several incompatible guarantees; treating
default identity as an open device; equating a render reference with audible PCM.

Do not force callbacks and workers into identical scheduling, expose native
handles, merge capture/render clocks or lifecycle, move speech processing into
core, or build arbitrary backend combinations for visual symmetry. Record public
contract changes as proposals with migration impact.

## Report

Include current architecture/dependency map; execution traces; findings with
source anchors; compatibility/evidence matrix (implemented, compiled, exercised,
qualified, incomplete, unsupported by design); concise ownership/affinity,
buffer lifetime, flow, timing, cancellation and retirement contract; incremental
recommendations; validation and changes made. Keep service/DSP work explicitly
outside the core while explaining the adapter boundary it needs.
