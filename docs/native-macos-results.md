# Native macOS physical streaming results

Follow-up: [additional native qualification](native-macos-followup.md) records
loaded duplex, callback/flush traces, aggregate removal/default changes, and a
rollback fix. This page preserves the first trial's historical scope.

Partial qualification on 2026-11-17; this is not full production acceptance of
[the native goal](native-macos-goal.md). Physical execution is now demonstrated.
The independent known-PCM, hotplug and calibrated physical-latency requirements
remain open. Historical [BlackHole evidence](coreaudio-qualification.md) is
unchanged and must be assessed separately.

## Host and method

Native arm64 macOS 26.6.2 (25G83), Darwin 25.6.0, Apple M1 Max, 32 GiB RAM.
Release uses AppleClang 17.0.0.17000604 and SDK 26.2 with command-line tools;
full Xcode is not installed. Instrumented execution uses Homebrew LLVM 22.1.8.
AC power configuration reports low-power mode enabled. No power settings,
priorities, default endpoints, volume or device nominal rates were changed.
The desktop and builds/tests ran concurrently during portions of the sustained
trial; this is ordinary desktop load, not a controlled stress benchmark.

The explicit endpoints are `MacBook Pro Microphone` (`BuiltInMicrophoneDevice`)
and `MacBook Pro Speakers` (`BuiltInSpeakerDevice`), built-in transport, each at
48 kHz and native period 512 frames. The Apple system audio driver comes with
this OS; separate firmware/driver version was not exposed by this test.
Microsoft Teams Audio was enumerated but never selected. Both built-in legs
are reported with unknown clock relationship; equal rates do not prove shared
clocks. Conversion facts remain separately available in negotiated stream info.

The opt-in [physical executable](../tests/coreaudio_physical.c) resolves exact
names to IDs from its own enumeration, embeds a dedicated microphone purpose
string, and requests authorization through AVFoundation in the **test host**.
Authorization changed from undetermined to authorized through normal macOS
controls. The library does not grant/request permission. Captured PCM is checked
in memory for finite/nonzero samples, then discarded; no audio is saved,
uploaded, monitored back to the speakers or put in evidence files. A quiet
375 Hz triangle at about -46 dBFS is submitted as playback. Device gain and
physical sound level are not measured. Playback stops at each process teardown;
no audio runs during documentation work.

Application quantum is 256; capture has 64 slots and playback 16. Playback is
fully primed before start. After one second of warmup, counters are baselined
for the requested nominal interval. Flush and stop/restart follow that interval.
A separate capture reader keeps draining while native control calls block.
The oracle checks logical continuity, gap flags and epoch changes (one expected
change at explicit restart), stale-generation rejection, lifecycle completion,
no additional Hail allocations during processing, and balanced destruction.
Later short trials also execute eight active reopen/close cycles in one context.
This validates transport and metadata, not sample-exact microphone content or
known playback-to-capture PCM.

## Results

Raw data, source/binary hashes and platform commands are under
[evidence/native-macos](evidence/native-macos/). The source base is
`3c126fb6f0a9a50dd5435b19059e3c84853fdc27`; per-trial hashes identify working-tree
sources. The sustained run predates the extra eight-cycle check; its executable
and source hashes match the original Release matrix snapshot.

| Trial | Result |
| --- | --- |
| Static Release mono F32 duplex | 600.001124 s nominal interval; zero sustained underruns and zero capture loss |
| Capture over whole sustained process | 28,875,264 frames; every F32 sample nonzero; zero gap observations; only the explicit restart changed capture epoch |
| Flush in sustained process | 58.130 ms observed acknowledgement; capture reader advanced 3,072 frames during the operation; stale writes rejected |
| Sustained Hail memory | 285,504 bytes peak context + stream, zero steady-state Hail allocations, balanced teardown |
| Sustained process resources | 16,580,608 bytes peak RSS; 4.394308 s user + 10.953748 s system CPU including warmup/transitions, roughly 2.55% of one core over the nominal 600 s |
| Capture timestamp to read | mean 10.438 ms, maximum 19.485 ms; p50 bucket 12 ms, p95/p99 bucket 13 ms (1 ms bins, whole process) |
| Maximum control poll | 71.457 ms across lifecycle operations; not a real-time-safe control deadline |
| Static Release matrix | Mono S16/F32 duplex at 48 kHz pass; other requested rates and stereo duplex fail explicitly |
| Shared Release | Stereo S16/F32 playback-only pass; mono F32 duplex checked separately |
| Capture-only | Mono S16 at 48 kHz passes |
| LLVM ASan/UBSan physical | Five-second mono F32 duplex and eight active reopen/close cycles pass, no diagnostics; instrumented timing kept separate |
| Other checks | Static 5/5 and shared 4/4 portable/native-handoff suites; installed C/C++ consumers 2/2 for each linkage; 27 exports for each linkage; Clang analysis of core/queue/native/CoreAudio clean |

Capture host-age is **software delivery from the supplied native timestamp**,
with unknown timestamp uncertainty, not calibrated ADC latency. CoreAudio uses
mach absolute time; the test converts current mach time into that same domain.
Histogram entries are bucket lower bounds; values >=1000 ms occupy an overflow
bucket. Capture frame counts include warmup, flush and restart. Resource and
host-age metrics precede the later extra reopen cycles. Startup underruns were
also zero in the sustained process; no transition underruns are silently merged
into the sustained counter. Flush acknowledgement includes polling and native
stop/reset/start; no per-SDK-call trace or audible-tail bound is claimed.

The device's current nominal rate remains 48 kHz. All requests at 16/24/44.1 kHz
failed `HA_FORMAT_UNSUPPORTED`, without changing the device or adding a resampler.
The microphone exposes one channel, so stereo duplex failed explicitly; stereo
speaker playback succeeds independently. This does not establish that the
hardware cannot operate at another nominal rate, only that Hail correctly rejects
a mismatch with its current configuration.

## Retained failures and limits

The first harness metadata query used the wrong CoreAudio UID translation
argument shape and failed before opening audio. It now uses the SDK-documented
CFString qualifier and AudioDeviceID result. An initial three-second trial passed
transport but produced invalid host-age values (~966,008 seconds) because the
new harness subtracted CLOCK_MONOTONIC from CoreAudio mach absolute timestamps.
That measurement was corrected; [initial observations](evidence/native-macos/initial-observations.txt)
retain the failure. No backend change was needed to make the physical trials pass.

AppleClang 17's ASan runtime hung before `main`, both inside and outside the
sandbox. A [stack sample](evidence/native-macos/apple-asan-startup-hang.txt) shows
recursive sanitizer initialization while enumerating the dyld shared cache and
allocating a block, stuck in StaticSpinMutex. The stalled processes were terminated.
The separate LLVM 22 ASan/UBSan build passes without disabling checks or suppressing
findings. This is a toolchain limitation, not a clean AppleClang sanitizer result.
LeakSanitizer and TSan physical qualification are not claimed.

No BlackHole device is installed. One strict fixture attempt is retained as a
failure in [the log](evidence/native-macos/blackhole-unavailable.log); it does not
open physical devices. Teams Audio was not substituted. Installing a driver on
this active desktop would not establish the isolated fixture required by the
goal. Existing CI BlackHole results remain the independent known-PCM evidence;
new local strict/instrumented fixture qualification is still unavailable.

Built-in endpoints cannot be physically unplugged. No removable USB/Bluetooth
pair was available/selected, so native removal/reconnection, independent-device
duplex and live default-change tests remain unqualified. Default routing was
left alone. Permission denial/revocation was not induced or bypassed; authorized
capture is the observed path. Callback execution/jitter traces, loaded long-run
trials, per-native-call flush cost, calibrated physical round trip and last
audible old sample require further instrumentation/fixtures. Acknowledgement
around 50–60 ms remains a measured limitation, with no claim about which native
subcall dominates. No public ABI or core DSP change is justified by these results.
