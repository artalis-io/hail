# Predeclared standalone WASAPI follow-up, 2026-11-19

Build `tools/probes/wasapi_native_repro.c` using only Windows SDK headers and
the C runtime; link only `ole32` and `uuid` in addition to the compiler's standard
runtime libraries. No Hail header, object, library or API belongs to this process.
Fixture validation may use the existing Hail validator in a separate preparatory
process; that process does not perform the native reproduction.

The native program opens only the exact approved VB-CABLE F32 stereo 48 kHz
capture/render pair, using shared mode, event callbacks, automatic conversion,
default-quality SRC, a 20 ms requested buffer and 256-frame render chunks. A
single COM owner services both legs, matching the existing adapter's SDK
ownership. It first drives silence for 1.5 seconds and verifies more than
24,000 exact zero capture frames in the final 900 ms, then stops/resets both
clients before beginning the fresh marker trial.

Generate the same independent low-grid A/B codes:
`left = (marker * 2048 + (serial % 2048)) / 32768`, right the exact negative.
Use one-second startup plus a five-second nominal A phase. Execute render-only
`Stop -> Reset -> Start` while capture stays running; optionally send silence
during an application-observation delay before producing B for one second.
Drain with silence for 400 ms, stop/reset, release all native resources and
report the original exact sample/marker/sequence checks. Save at most eight
fractional synthetic examples per cumulative phase; retain no bulk PCM.

Exactly 16 trials, once each, in this nesting order:

1. Primed, then unprimed startup.
2. Additional requested pre-flush delay: 0, 3, 7, 11 ms.
3. Requested post-restart delay before B: 0, 16 ms.

Primed native render fills only its actual SDK buffer before start. There is no
Hail application queue, control poll, reference queue, stale-write API or
generation machinery. Actual buffer sizes and elapsed control/observation
delays are reported; requested delays do not imply exact engine phase alignment.
Unprimed starts with no marker priming and observes a 60 ms silent interval.
Thus this is a reduced SDK reproduction, not an identical scheduling model or
a replacement for Hail qualification. Each process performs its own explicit
silence conditioning. No failure is retried automatically and there is no long
run. All-zero capture cannot pass the A/B verified-frame minima. The zero
sample tolerance is unchanged.

A fractional SDK capture value with exact submitted native PCM is sufficient
to reproduce the alteration without Hail. It does not identify the responsible
Windows/driver/cable component or prove an SDK defect. If none of the 16 cases
reproduce it, retain that bounded negative result and return to the observed
control/scheduling differences; do not retry until a desired result appears.

After the reproduction, apply the repository C and architecture-axis audit
workflows to source/public-private boundaries, native ownership/retirement,
tests/fuzz/build/CI and the new probe. Report platform execution limits and any
findings separately from this audio fixture's numerical behavior.
