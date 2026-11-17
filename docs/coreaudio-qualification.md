# CoreAudio virtual-device qualification

Separate [native physical results](native-macos-results.md) cover the built-in
microphone/speakers on a native M1 Max Mac. Those observations complement this
virtual known-PCM fixture; neither report substitutes for the other's scope.

Native CoreAudio loopback is now exercised on an actual macOS runner with
BlackHole 2ch 0.7.1. The tested host reports macOS 26.7.2, arm64. The driver is
an independent fixture installed by Homebrew on a disposable runner, not a
Hail dependency. No physical microphone/speaker or default-device routing is
used, and no privacy database or OS security setting is modified.

[CI run 43976411781](https://github.com/artalis-io/hail/actions/runs/43976411781)
passes the static/shared Release suites and three native ASan/UBSan repetitions.
[Structured evidence](evidence/coreaudio-loopback.json) includes driver version/
hash, platform, source hashes, measurements and earlier failed profiles.
[The C test](../tests/coreaudio_loopback.c) and
[manual workflow](../.github/workflows/coreaudio-loopback.yml) reproduce the run.

## Verified scope

The fixture is explicitly selected by its BlackHole identity and configured to
48 kHz. Application PCM is stereo interleaved F32 with quantum 256, 64 capture
slots and eight playback slots. A counting allocator observes Hail allocation
and destruction. Distinct exactly representable values encode channel, frame
sequence and playback generation; all-zero capture cannot pass.

The strict Release profile verifies nonzero duplex PCM, ordering and capture
frame/clock continuity through native playback flush, rejects stale writes,
stops/restarts with a new generation/epoch, and closes while native servicing
is active before proving balanced destruction. It does not test driver removal,
other hardware, other PCM formats/rates, or physical microphone permissions.

The latest eight Release processes verified 1,124,352 nonzero frames with zero
bad samples/order, capture frame gaps or capture queue loss. No steady-state
Hail allocations occurred; peak Hail-requested storage was 351,168 bytes.
Observed flush acknowledgement ranged from 34.420 to 100.594 ms. Cumulative
playback underrun counts ranged from 512 to 7168 frames; these include unprimed
startup/restart and test transitions. They are not a measured steady-state-only
underrun rate. Silence is counted separately.

Those acknowledgement times include control polling and native stop/reset/start.
They are not an audible interruption-tail bound. Capture continues independently,
but native control calls can block the control owner. No capture-to-consumer or
physical ADC/DAC latency benchmark is claimed by this test.

## Instrumented profile and retained failures

The ASan/UBSan build instruments Hail's native callback/lifetime paths and the
C test. Apple frameworks and the separate driver/server are not instrumented.
No LeakSanitizer, TSan or hard-deadline qualification is inferred from this run.
The latest three instrumented processes passed without ASan/UBSan diagnostics,
bad samples/order, or observed clock/sample gaps; their underrun counts remain
in the report. Their stricter timing result is an observation, not a guarantee.

Earlier runs failed on a capture epoch discontinuity and then a delayed forward
PCM-counter jump under instrumentation. Diagnostics showed the source-counter
jump 1024 capture frames after a clock discontinuity. The original oracle had
consumed that discontinuity on an intervening intact sample. The revised
instrumented oracle retains a reported-event credit until a forward sequence
gap occurs; it does not require buffered-route effects at the first marked
sample. It still rejects jumps without a reported-event credit, bad values/channels
and old-after-new generations. A credited jump must advance the modular frame
sequence by less than half its 65536-frame range. All accepted gaps are logged/countable.

This is a separate instrumented/load profile, not a relaxation of the strict
Release continuity requirement or proof of the exact underlying scheduling/
driver cause. The retained failures remain evidence of timing sensitivity.
No sanitizer diagnostic is suppressed and no production backend code changed
to obtain these results. Physical/long-duration real-time validation and
interruption performance remain Gate E work.
