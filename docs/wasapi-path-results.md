# WASAPI stage isolation and exact low-grid follow-up, 2026-11-18

The [original qualification failures](native-windows-paired-results.md) were
isolated with independent native Windows clients. High-level changes are
already present at Windows render-loopback output, before the cable capture
leg. S16 conversion noise is present in native S16 capture independently of
Hail. A separate, predeclared low-amplitude exact F32 profile has passing short
and native ASan trials after one harness boundary correction, but a later
conditioned unprimed case failed on fractional samples during native flush.
The first ten-minute attempt also failed its final oracle assertion despite
zero measured transport loss; a second was blocked by the failing short case.
**No complete paired profile or ten-minute numerical pass is claimed.**

## Independent stage diagnostic

`hail_wasapi_path` is an explicit native diagnostic, built alongside the native
qualification executable. It writes stereo samples `(v, -v)` through Hail and
simultaneously observes five paths: Hail render reference, native F32 Windows
render loopback, native F32 cable capture, native S16 cable capture, and Hail
capture. The native clients have their own COM owner and never access Hail's
worker-owned interfaces. The entire application-side diagnostic runs on one
thread, respecting each Hail I/O/control role. No PCM recording is created.

The eight fixed S16-unit levels are `0, 8192, 16384, 24576, 30000, 32000, 32700,
0`. Each phase lasts 1.5 seconds; its first 600 ms is excluded to allow queued
constant-level transitions to settle. The remaining samples are compared with
their exact constant expected values, with zero numerical tolerance. This
settling window is specific to a static diagnostic and is not a relaxation of
the dynamic qualification's generation/continuity checks. Two explicitly
selected F32/S16 Hail trials completed; diagnostic completion is not a numerical
pass and does not certify ordering or latency.

| Observation | Independent result and implication |
| --- | --- |
| Hail render reference | Exact at every tested level for both Hail representations, including 32700. |
| Native render loopback F32 | Exact through 32000; all 43,200 measured frames at 32700 changed, maximum error 423.5 S16 units. The high-level alteration is downstream of Hail reference and observable before the cable capture endpoint. |
| Native cable F32 | Exact through 32000; the 32700 phase has the same 423.5-unit maximum error. |
| Native cable S16 | About 40–41% of measured frames differ at levels through 32000, including silence; maximum error 1 S16 unit. At 32700 the maximum error is 425 units. |
| Hail capture | F32 follows the F32 path's results; S16 has the same per-phase counts and maximum errors as the independent native S16 observation. This is a comparison of aggregates, not a claimed per-frame trace match. |

This establishes where the alterations first become observable. The high-level
behavior is consistent with render-engine limiting, but the specific Windows
DSP implementation was not identified. The S16 behavior is consistent with
dithering/quantization in the shared capture representation conversion. It
does not justify accepting arbitrary PCM or fitting the exact oracle's tolerance
to these observations. The cable itself is not shown to introduce an additional
transformation at the exact lower F32 levels.

Microsoft's [GetMixFormat contract](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-getmixformat)
distinguishes the engine's internal format from device formats. Built-in
[render loopback](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording)
captures the render endpoint; it is used here to locate a stage, not as a
replacement for Hail's capture-client qualification.

## Exact low-grid profile and demonstrated harness correction

The [low-grid contract](wasapi-low-grid-profile.md) was committed at `31fb55e`
before its first native run. It uses integer S16-grid codes 2048–8191, an 11-bit
sequence, opposite stereo channels, and three independent generation markers.
No tolerance is allowed. This deliberately different signal does not convert
the original near-full-scale test into a pass.

The initial five-second low-grid F32 trial had exact values but one sequence
failure in residual B samples after stop/reset. The old native and Hail queues
are deliberately discarded by stop; continuity of old B across that discard
is not promised. The corrected oracle checks every residual tail value and
channel, preserves monotonic marker order, and records tail frames separately
without giving them verification credit. From the first C frame onward,
skipped C samples and any old-after-new B still fail. The correction does not
change steady-state or render-only flush continuity assertions.

Both original and low-grid device-free oracle tests independently exercise
corrupt tail samples, post-C skips and old-after-new samples. The dynamic
five-second low-grid F32 primed/unprimed cases then pass with zero bad counts,
zero measured capture/render/reference loss, no measured or flush capture
continuity changes, and two allocations/two frees. LLVM 23 ASan also passes the
five-second strict native low-grid F32 case. The sole reported capture metadata
gap in each complete trial belongs to the intentional overflow phase and is
retained rather than hidden.

S16 low-grid primed and unprimed cases were explicitly attempted and both failed
the exact verified-A/B minimum assertion; first-error diagnostics show changed
S16 samples. No S16 numerical conversion oracle or passing S16 exact paired
result is claimed. A suitable independently specified conversion oracle or
different transparent fixture configuration remains required for that claim.

## Cross-trial contamination and remaining flush failure

The first sustained attempt completed a `600.0205991` second measured phase
with zero capture loss, playback underruns, reference loss, capture metadata
gaps and capture epoch changes; its flush metadata counters were also zero.
It nevertheless failed the verified-A/B minimum assertion before restart and
balanced teardown. Thus it is a completed measured phase, not a passing complete
qualification trial or proof of bit-exact sustained sample delivery. It followed
a failed S16 test that exited while B was still being produced. Its A/B counts
were not printed by that build, so the precise original counter values are not
invented here.

A bounded reproduction deliberately ran failed S16 followed immediately by F32.
The added diagnostics showed stale B first, then rejected A as an old-after-new
regression: verified A was zero and `bad=97921`. Opening new Hail clients does
not clear nonzero PCM already queued downstream in the cable. This reproduced
setup defect explains why independently launched trials require conditioning;
it is consistent with the preceding sustained failure but not a substitute for
its missing numerical counters.

`hail_wasapi_path ... f32 --condition` now drives 1.5 seconds of silence and
requires more than 24,000 measured exact-zero frames at Hail reference, native
render loopback, native F32 cable capture and Hail capture after its 600 ms
setup interval. That preflight is separate from the qualification. It neither
discards early samples inside the dynamic oracle nor forgives regressions.
The native S16 observer remains noisy and is not part of this F32-silence claim.
The conditioning step and the conditioned primed short case passed, including
ASan execution of conditioning.

The conditioned unprimed case then found a different failure: 479 bad samples
in the native-flush phase, including `(3221.375, -3221.5)` followed by a descending
ramp in S16 units. Verified A/B were 287905/41658; the final exact `!bad` check
failed. No second ten-minute trial was started because this short precondition
failed. The qualification runner now enforces that same F32 startup-case gate.

Additional constant-flush diagnostics held an odd amplitude of 8193 S16 units
to expose fractional gain changes. These ran with and without an active native
render-loopback monitor. They observed flush-induced zero intervals, but no
fractional F32 samples at any observed stage. The no-loopback case also
completed under ASan. These different observations **do not locate or excuse
the ramp**. Its origin remains unresolved; engine de-clicking is a hypothesis,
not a demonstrated cause. No tolerance, fractional-sample exception or
retry-until-pass loop was introduced. The current exact profile remains
unqualified at native transitions despite its passing short samples.

## Reproduction and limits

Validate the saved signed-fixture manifest first, with a new real reservation.
Only use its approved exact endpoints. Example stage invocation:

```powershell
./build-windows-validation/OFF-Release/hail_wasapi_path.exe `
  '{0.0.1.00000000}.{87d84b71-17b7-4d13-89df-b4b95b98aa6d}' `
  '{0.0.0.00000000}.{266bf420-96a2-47b5-a831-f6a6dbc0ae03}' f32
```

For the separate dynamic profile, `hail_wasapi_loopback` accepts `--low-grid`
alongside the same `--capture`/`--playback`, `--pcm f32`, `--seconds 5` or 600,
and optional `--unprimed`. The provisioned qualification runner now maps a
`transparent-s16-grid-low-v1` manifest to that flag; its ordinary full matrix
still fails for missing profiles and failed S16 cases. It never equates a
passing F32 subset with passing the full qualification.
The stage helper's optional `--flush` and `--flush-no-loopback` modes diagnose
native reset using a constant odd-amplitude signal; an inactive monitor is
reported with zero observed frames rather than claimed as evidence.

All four static/shared Debug/Release build/consumer/export configurations pass;
static configurations execute 11 device-free tests and shared configurations
execute seven. Matching LLVM 23 ASan executes all 11 device-free tests. Native
audio under other rates/channels and S16 exact transparency remain unqualified
by this follow-up. Physical latency and audible-tail bounds remain unmeasured.
No production backend, public API, default audio role, microphone setting,
driver/security configuration or monitoring route is changed by this work.

Raw constant diagnostics, failed initial low-grid trial, corrected short trials,
ASan/build checks, manifest validation, and sustained-trial telemetry are retained
in the [raw archive](evidence/wasapi-path-2026-11-18/raw.zip) with a
[hashed evidence index](evidence/wasapi-path-2026-11-18/summary.json).
The device-free runner-gate integration cases also pass: rejecting an F32
startup failure blocks sustained execution; passing F32 startup allows its
separately scoped sustained process while S16/missing-profile failures still
reject the full aggregate. These mock processes never open audio devices.
Final audio-default roles and muted Shure level readbacks match the original
session readbacks, and no Hail audio process remains running.
The initial constant diagnostic build
did not record its executable hash; its audio-path source is captured by
`31fb55e` (later changes add its device-free observer test, defensive bounds,
explicit conditioning and flush diagnostic modes).
The first diagnostic invocation failed its own missing poll-count argument
before observations; that failure is also retained.
