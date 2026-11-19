# Standalone WASAPI result, 2026-11-19

**All 16 predeclared native-only trials passed. The fractional ramp did not
reproduce.** This is a bounded negative result, not a completed Hail paired
transition qualification or proof of a Windows defect.

The [plan](wasapi-native-only-plan.md), source and runner were committed at
`83299c9` before execution. Every primed/unprimed × requested pre-flush delay
0/3/7/11 ms × requested post-restart observation delay 0/16 ms case ran once.
There were no retries, tolerance changes or ten-minute runs. Execution completed
at 2026-11-18 23:45:40 UTC (2026-11-19 locally).

## What ran

[`wasapi_native_repro.c`](../tools/probes/wasapi_native_repro.c) uses Windows SDK
interfaces and the C runtime directly. It includes no Hail header and links no
Hail object/library. Retained PE imports and undefined object symbols verify
that separation. Fixture validation used Hail in a separate preparatory process.

The approved signed VB-CABLE 3.5.2.0 pair used shared F32 stereo at 48 kHz,
event callbacks, automatic conversion and default-quality SRC. Each leg reported
a 1,056-frame native capacity for the requested 20 ms buffer. Each process
conditioned and checked silence, submitted independent exact low-grid A/B
markers, performed render-only Stop/Reset/Start with capture continuing, checked
exact capture samples and marker order, then stopped/reset and released its
resources. At least 16,000 verified frames of each marker were required; silence
alone could not pass. Requested delays were measured, not assumed exact.

| Check | Result |
| --- | --- |
| Fixed numerical matrix | 16/16 pass, no watchdog expiry |
| Submitted render samples | Zero fractional, asymmetric or nonfinite samples |
| Captured marker samples | Zero fractional, asymmetric, nonfinite or bad samples |
| Separate ASan native profile, primed/7/16 | Pass; resources released; no reported ASan error |
| Native oracle negative cases | Pass in all four Windows build configurations and ASan |
| System defaults and Shure gain/mute readbacks | Identical before/after |

No microphone PCM was opened or retained by this reproduction. The saved
output contains synthetic aggregate observations, bounded synthetic examples,
fixture configuration and build/test evidence; it contains no bulk PCM. The
separate ASan trial is excluded from the 16-case numerical matrix.

## What the result means

The earlier [SDK-boundary observation](wasapi-marker-results.md) still stands:
the failing capture lease contained fractional values before Hail copied them,
while the submitted render lease was exact. That establishes the observed
alteration between SDK render submission and capture delivery. It does not
identify the Windows engine, driver or cable component responsible.

This standalone program reduces the execution model. It has no application
queue, generation handoff, control polling or reference consumer. Priming fills
the actual SDK render buffer; Hail can also have application PCM queued. Its
single owner services render opportunistically during the event/poll loop,
whereas Hail's worker services the render event alongside commands and capture.
Those timing and occupancy differences remain plausible investigation targets;
this campaign does not establish a causal explanation.

The bounded follow-up is complete. No vendor issue was filed and no additional
audio matrix was started. Exact Hail paired flush transparency remains
unqualified. Any further experiment should begin with a specific scheduling or
queue-occupancy hypothesis and a new predeclared plan.

## Reusable evidence

[Summary](evidence/wasapi-native-only-2026-11-19/summary.json),
[case table](evidence/wasapi-native-only-2026-11-19/matrix.csv),
[raw evidence](evidence/wasapi-native-only-2026-11-19/raw.zip), and
[standalone source package](evidence/wasapi-native-only-2026-11-19/standalone.zip)
retain arguments, endpoint identities, source/executable hashes, output hashes,
timestamps, watchdog outcomes, measured native controls and build/import checks.
The source package includes a build/run README and this negative-result report;
it contains no vendor driver binaries or Hail library.

The subsequent [C audit](c-audit-2026-11-19.md) and
[architecture audit](axis-audit.md) record source review and platform limits.
