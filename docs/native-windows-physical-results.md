# Shure native Windows qualification, 2026-11-18

The owner authorized playback and microphone capture on the Shure MV7+.
Physical capture, playback and duplex now execute through Hail's WASAPI backend.
All 32 strict duplex format/startup cases passed after a capture-clock fix:
16/24/44.1/48 kHz, mono/stereo, S16/F32, primed/unprimed. Standalone capture and
playback, static/shared Debug/Release duplex, 30 lifecycle cycles and two native
ASan profiles also passed. This qualifies those recorded operations/configurations;
the complete [Windows goal](native-windows-goal.md) remains open for the signed
paired fixture and actual removal/reopen. Physical latency is uncalibrated and
is not claimed by this pass.

The final strict Release trial completed 600.0092291 seconds of measured duplex
with meaningful input: zero capture loss, playback underruns, reference loss,
logical capture gaps or capture epoch changes. Playback flush also preserved
capture continuity. [Measurements](evidence/native-windows-physical-2026-11-18/measurements.json)
and [exact raw logs](evidence/native-windows-physical-2026-11-18/raw-trials.zip)
retain successful and failed campaigns separately.

## Host, devices and settings

Backend fix: `1586244aec5e369726d5c3a7f1ef3d7ccbc71d8e`. Release builds use ClangCL
19.1.5 and Windows SDK 10.0.26100; diagnostics use an isolated official LLVM
23.1.6 toolchain. Host: Windows 11 Pro x64 build 26300.9974, AMD Ryzen 9 5950X.
The Shure uses Microsoft's USB Audio 2.0 driver 10.0.26100.9974. Native endpoint
queries report a 10 ms default period and 3 ms minimum. The independent query
client observed 1,056 application frames at 48 kHz for a 20 ms buffer request;
that query is not direct instrumentation of Hail's worker-owned buffer.

| Leg | Exact persistent endpoint key | Shared engine format |
| --- | --- | --- |
| Capture: Microphone (2- Shure MV7+) | `{0.0.1.00000000}.{bf16f049-5379-4d2e-a4d9-a15a142424d8}` | 48 kHz mono F32 |
| Playback: Headphones (2- Shure MV7+) | `{0.0.0.00000000}.{6e8c8429-b7e1-4431-b05c-86a29ef73477}` | 48 kHz stereo F32 |

The first tests found the input muted with gain zero; F32 contained only very
small values (peak about -135 dBFS). Those results establish transport, not useful
microphone pickup. Authorized later tests temporarily unmuted this exact input
with requested scalar gain 0.5 and restored each trial's saved settings in
`finally`, including after failures. Readbacks/settings records are retained;
some later saved gain values were about 0.21. Playback endpoint scalar was about
0.98; the synthetic signal peak was 504/32,768, below -36 dBFS. No defaults,
microphone privacy policy, system security, driver installation, thread priority,
MMCSS, timer resolution or power policy were changed. No microphone PCM was saved.

All successful post-unmute capture cases exceeded a predeclared -90 dBFS peak
indicator and delivered finite nonzero PCM. That indicator distinguishes useful
input from the earlier near-zero values; it is not a calibrated acoustic level,
intelligibility test or physical channel/latency oracle. Stereo application
capture uses Windows conversion from the mono engine. Physical device format and
shared hardware clock relationships remain unknown.

## Defect and retained failures

The first format campaign passed all eight 48 kHz cases and failed the 24 lower-rate
cases. At 16 kHz, the native capture packet position advanced by 480 engine frames
while a normal packet delivered 160 application frames. The adapter had marked
the position valid, and `native_clock` compared it against application-frame
counts. Every packet then appeared discontinuous, generating false unknown gaps
and epoch changes. Diagnostic event/log traffic also accompanied xruns in those
failed runs. The partial first resampler packet prevents an exact integer
position rescaling from being assumed.

`wa_clients_open` now reads the capture client's actual engine rate on its COM
owner. With differing rates, `wa_capture` leaves raw device position invalid and
marks the SDK host correlation estimated, with unknown uncertainty. Matching
rates retain device-frame validity. A device-free regression checks the flags
and application timing denominator. No resampler, DSP, SDK type or new public
ABI entered portable code. The complete 32-case campaign then passed without
changing PCM tolerances or discontinuity rules. This is a native timing-fact fix,
not proof of bit-exact Windows numerical conversion.

The first standalone capture attempt also exposed a test configuration bug:
the harness supplied playback queue slots while playback was disabled. Core
correctly rejected the plan. The harness now sets disabled-leg slots to zero;
standalone capture and playback passed afterward. That rejected case is retained.

## Executed checks and interpretation

| Case | Result and scope |
| --- | --- |
| Strict application format/startup matrix | 32/32 passed; each has 1 s startup settling and 1 s measured steady phase, plus flush/restart/starvation/overflow phases |
| Native flush | No steady/flush capture logical gaps or epoch changes in the successful matrix; stale generation rejected; request-to-observed acknowledgement 16.7544–19.6048 ms, median 18.6115 ms |
| Standalone capture/playback | Passed at 48 kHz stereo F32; playback observes successful native submission/reference, not independently measured DAC output |
| Native lifecycle stress | 30 passed: 10 stop-during-start, 10 close-during-start, 10 stop-during-flush; 31 allocations/31 frees, zero retained Hail bytes |
| Static/shared Debug/Release | All four native duplex samples passed; portable tests 9/9 static, 5/5 shared, installed C/C++ consumers 2/2 each, exact 27-symbol checks |
| Deliberate faults | Capture newest-drop loss/cumulative gap metadata and playback underrun/reference silence required and observed; excluded from steady statistics |
| Memory diagnostics | LLVM 23 ASan device-free 9/9 plus native 48 kHz stereo F32 and 16 kHz mono S16 passed; no sanitizer suppression |
| ASan control | Independent clean probe exited zero; intentional 8-byte heap overrun failed with a symbolized ASan report |
| Retirement | Application I/O joins before final close; native worker exit proof and successful context destruction; exact sized/aligned allocator ledger balanced |
| Timing | Independent Hail leg domains; converted capture raw position invalid/host estimate explicit; native render reference is not a DAC timestamp |

The final ten-minute case used 620,272 Hail bytes, made only its two construction
allocations and balanced both frees at teardown. Total capture across the full
trial, including transitions/faults, was 29,002,656 frames; peak input was
-64.55 dBFS. Observed native flush acknowledgement was 17.7688 ms. Deliberate
capture overflow dropped 10,560 frames; deliberate playback starvation added
17,120 underrun frames and produced silence. Neither injection belongs to the
zero-xrun measured steady phase.

Measured process CPU during the 600 s phase was 1.546875 s, about 0.258% of one
logical CPU. In 599 process samples, working set ranged 11,141,120–11,317,248
bytes, private bytes 2,441,216–2,592,768 and handles 195–200. Windows audio-service
CPU/memory is outside this process measurement. The ordinary desktop also ran
source review/static analysis; this is not an isolated or hard-real-time bound.

Two separate strict 5 s timing profiles record SDK capture-correlation-to-read
observations in 100 microsecond buckets. **These are not calibrated physical
latencies.** At 48 kHz F32, 826 observations were nonnegative and 178 SDK
correlations were later than the application read time. The nonnegative subset's
p50/p95/p99 bucket upper bounds were 6.7/14.3/15.3 ms. At converted 16 kHz S16,
all 503 observations were nonnegative and explicitly estimated; corresponding
upper bounds were 7.5/14.8/15.6 ms. The future correlations are retained, not
discarded from the report or presented as valid nonnegative delivery latency.
Their unknown timestamp uncertainty prevents a physical latency interpretation;
the conditional quantiles do not describe all 48 kHz observations. Resolving
physical timing accuracy requires an independent calibrated fixture.

LLVM 19's earlier interceptor-initialization crash is preserved. LLVM 23 was
downloaded/extracted only under an ignored build directory; archive SHA-256
`7623f582089983c1cb51030cf1996514b19464e5669099571b399e8543770155`
matched the official release asset metadata. Compiler and ASan runtime match.
These are Windows ASan results, not Windows UBSan/TSan/LSan coverage. Process
memory excludes the separate Windows audio service/driver allocations; the Hail
allocator ledger does not claim those components allocate nothing.

Restart has reported native capture discontinuities, and the deliberate overflow
has a known logical gap with loss metadata. Those are separate transition/fault
phases, not accepted sustained xruns. Native playback flush itself did not cause
capture epoch/logical-position changes in the successful strict trials. Equal
requested capture/render rates do not imply a shared clock.

## Reproduction and remaining requirements

Use [harness instructions](wasapi-qualification.md). `run_wasapi_physical.ps1`
requires exact selected keys and prior audio authorization; `-TestCaptureGain 0.5`
explicitly enables temporary input-level changes and restores saved settings.
It writes raw stdout/stderr, per-trial executable hashes, results and periodic
process CPU/private-byte/working-set/handle observations. The recorded ordinary
`Sleep(1)` application loop commonly wakes at 14–16 ms; no hard-real-time claim
or physical latency bound follows from the observed software timings.

Actual USB unplug/replug requires owner coordination; the explicit
`run_wasapi_physical_hotplug.ps1` test is prepared and has not run. No device was
disabled administratively. A signed, reserved, transparent paired fixture with
approved provisioning/manifest is still unavailable. The existing MOTIV virtual
endpoints were not treated as a verified cable. Hardware permissions/revocation,
old-after-new PCM across a paired path, physical ADC/DAC latency and audible tails
remain separate evidence gaps. Do not describe this pass as blanket production
readiness or completion of every criterion in the goal.
