# Building and integrating Hail

The portable core and virtual backend have no external dependencies. CMake 3.16
and a C11 compiler with always-lock-free 32-bit atomics are required. Tests also
need a C++11 compiler and thread library. Neither is required for the library
when `HA_BUILD_TESTS=OFF`. Native adapters add only their selected OS dependencies.
CoreAudio and WASAPI default on for their OS; Linux backends are opt-in.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/hail_virtual_example
```

The example enumerates devices, primes PCM playback, captures and renders
simultaneously, interrupts playback while capture continues, rejects stale
playback, and handles disconnection. It uses a fixed arena, no audio hardware
and no wall clock. The native example enumerates by default; explicit endpoint IDs enable capture
and silent playback. It prints counters, interrupts playback while capturing,
and keeps no recording. See native commands below.

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DHA_BUILD_SHARED=ON
cmake --build build-release --parallel
cmake --install build-release --prefix /your/install/prefix
```

Only install to a prefix you own. CMake exports `Hail::hail`:

```cmake
find_package(Hail 0.1.0 EXACT CONFIG REQUIRED)
add_executable(my_audio_app main.c)
target_link_libraries(my_audio_app PRIVATE Hail::hail)
```

Static builds propagate `HA_STATIC` automatically through the CMake target and
pkg-config metadata. Manual static consumers define it before including the
header. The shared library has ABI/soname zero and an exact public export list;
version 0.1 does not promise drop-in replacement compatibility. No platform or
third-party type enters the public header. Installed records are ordinary
in-process C records, not serialization formats.

Use `-DHA_SANITIZER=address`, `thread`, or `undefined` in separate GCC/Clang build
trees. Address mode enables ASan and UBSan. This host's GCC LeakSanitizer required
execution outside its ptrace-based sandbox. GCC 13 TSan could not initialize on
this kernel; Clang 18 TSan passed with the installed GCC 13 C++ libraries selected:

```sh
cmake -S . -B build-tsan-clang -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/13 \
  -DHA_SANITIZER=thread -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsan-clang --parallel
ctest --test-dir build-tsan-clang --output-on-failure
```

That GCC installation path is host-specific, not a Hail build requirement.
No runtime error suppression, retry-until-pass logic, or disabling of sanitizer
findings is part of the build.

The optional device-free fuzz suite uses Clang libFuzzer with ASan/UBSan:

```sh
cmake -S . -B build-fuzz -DCMAKE_C_COMPILER=clang -DHA_BUILD_TESTS=OFF \
  -DHA_BUILD_EXAMPLES=OFF -DHA_BUILD_FUZZER=ON -DHA_SANITIZER=address \
  -DHA_BACKEND_COREAUDIO=OFF -DHA_BACKEND_WASAPI=OFF \
  -DHA_BACKEND_ALSA=OFF -DHA_BACKEND_PIPEWIRE=OFF
cmake --build build-fuzz --parallel
python3 tools/run_fuzz.py --runs 100000
```

See the [fuzz suite](../fuzz/README.md) for configuration, lifecycle, queue-model
and backend-failure targets, named seeds, bounds, CI campaigns and crash replay.
Both the library and harnesses are instrumented. Native backends must be disabled.
On this Linux host Clang also needed the GCC installation selection shown above
in `CMAKE_C_FLAGS`, because libFuzzer links a C++ runtime. Normal C library builds
do not require a C++ runtime. Portable fuzzing does not qualify native drivers.

For reproducible builds, retain the exact source revision, CMake cache, compiler,
archiver and system SDK versions, feature flags and source/dependency hashes.
Use a fixed build path or compiler prefix-map flags for debug path reproducibility,
and a deterministic archiver. No byte-identical cross-toolchain claim has been
validated. There is no network dependency resolution in CMake.

## Native builds and examples

On Linux install the development packages for the adapters selected (for example
`libasound2-dev` and `libpipewire-0.3-dev` on Debian/Ubuntu), plus pkg-config and
pthreads. PipeWire 0.3.65 or later is required. CMake never downloads dependencies.

```sh
cmake -S . -B build-native -DHA_BACKEND_ALSA=ON -DHA_BACKEND_PIPEWIRE=ON \
  -DHA_BUILD_NATIVE_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-native --parallel
ctest --test-dir build-native --output-on-failure
./build-native/hail_native_example 4
./build-native/hail_native_example 5
```

Backend numbers are CoreAudio 2, WASAPI 3, PipeWire 4 and ALSA 5. Enumeration-only
commands open no audio stream. Add `CAPTURE_ID PLAYBACK_ID` to explicitly run a
short in-memory capture/silent-playback test; zero disables that direction. Use
the IDs printed by that invocation's enumeration, never saved indices. The test
reports capture after flush and epochs. For example, use an ALSA null pair or a
PipeWire dummy sink/monitor pair for testing without microphone access. Native
CTest integration opens only ALSA null; it skips explicitly if unavailable.
Tests never silently choose real input hardware.

On macOS use the ordinary build command; CoreAudio/AudioUnit/CoreFoundation/
AudioToolbox are linked from the SDK. Host applications supply microphone usage
description and authorization. On Windows use a C11 atomics-capable toolchain,
for example `cmake -S . -B build -T ClangCL`; WASAPI links ole32/uuid. MinGW-w64
is also compile/link tested. Disable native defaults explicitly for virtual-only
builds with `-DHA_BACKEND_COREAUDIO=OFF -DHA_BACKEND_WASAPI=OFF`.

Static installed CMake targets discover selected private dependencies; pkg-config
`--static --libs hail` carries native libraries. Shared consumers need the runtime
library and its OS dependencies. See [native contracts](native-backends.md) for
conversion, permissions, global PipeWire initialization, and retirement semantics.

## Freestanding and feature exclusion

```sh
cmake -S . -B build-freestanding -DHA_FREESTANDING=ON \
  -DHA_BUILD_TESTS=OFF -DHA_BUILD_EXAMPLES=OFF -DCMAKE_C_FLAGS=-ffreestanding
cmake --build build-freestanding --parallel
```

Provide aligned allocate/free callbacks backed by an arena/pool. There is no
default malloc/free implementation in this profile. The target supplies
freestanding memory functions such as memcpy/memset/strlen, a C11 atomic
implementation satisfying the compile gate, and its own startup/linker support.
The virtual backend requires external stepping, not a host thread or clock.
Disabling `HA_BACKEND_VIRTUAL` removes its object and public exports/header from
the install; with no native backend there is then no usable device driver and
context creation returns UNSUPPORTED. This profile proves exclusion/compilation,
not functioning RTOS, DMA or UEFI audio.

DMA buffers, cache maintenance, interrupt-safe atomics and device drivers belong
in future target adapters. Do not use blocking allocator hooks or thread locks
inside processing entries. Hardware interrupts must not invoke the current
public virtual/control driver concurrently. The portable core creates no
threads, accesses no files, opens no sockets and reads no OS clock.

## Structural and namespace checks

```sh
python3 tools/check_contracts.py build/libhail.a
python3 tools/check_contracts.py build-release/libhail.so --shared
python3 tools/audit_namespace.py ../keel ../hull ../bitnet.c > /tmp/sibling-symbols.json
```

The contract checker uses GNU-compatible nm on Linux; Mach-O and PE export
validation use target tooling: Windows needs `llvm-nm` for static COFF and
`llvm-readobj` for DLL exports. The sibling
audit remains incomplete for unavailable Sail and Hull binary artifacts. Static
archives may contain private `hail_*` helper definitions; only `ha_*` functions
listed in the export manifest belong to the public interface. Shared builds
hide all helpers. Source acquisition and builds make no network requests.

## Isolated native loopback qualification

`HA_BUILD_NATIVE_TESTS=ON` with PipeWire enabled builds `hail_pipewire_loopback`
and registers a real-server integration test. It requires Python 3, `pipewire`,
`pw-dump`, `pw-link` and `pw-cli` (Debian/Ubuntu: `pipewire`, `pipewire-bin`,
`libspa-0.2-modules`). It creates a private runtime directory, starts a server
with only a null sink/monitor and dummy clock, links Hail's native streams, and
removes the fixture afterward. No session manager, ALSA device discovery,
physical microphone/speaker, recording or user-server reconfiguration is used.

```sh
ctest --test-dir build-native -R pipewire_loopback --output-on-failure
python3 tools/run_pipewire_loopback.py \
  --test build-native/hail_pipewire_loopback --iterations 10 \
  --output /tmp/hail-loopback.json
```

The test checks exact nonzero stereo PCM, ordering, capture continuity and epoch
through native playback flush, stale writes, native endpoint disappearance,
replacement identity and complete reclamation. It measures application write to
capture-read time and observed flush acknowledgement, plus client CPU, process
RSS high-water and Hail allocator counts. The fixture fixes graph rate/quantum;
these are software-loopback measurements, not physical or hard-real-time latency.
The isolated client omits the RT module and DBus fallback, so its leak-clean
result must not be generalized to default desktop PipeWire configuration.

To reproduce the dependency leak investigation, build
`tools/probes/pipewire_context.c` with `-fsanitize=address` and the system
`pkg-config --cflags --libs libpipewire-0.3` flags, then run
`python3 tools/probes/pipewire_leaks.py --probe /path/to/probe --output /tmp/leaks.json`.
This compares otherwise identical client configurations with the RT module
absent, present with default fallback, and present with RTKit/portal disabled.
It reports failures rather than suppressing LeakSanitizer diagnostics.

## CoreAudio virtual-device qualification

The manual `CoreAudio virtual-device loopback` CI workflow installs BlackHole
2ch on a disposable macOS runner and exercises native static/shared Hail builds.
BlackHole is an independently installed test fixture, never a Hail build/runtime
dependency or vendored source. Its upstream license and installation details are
at https://github.com/ExistentialAudio/BlackHole.

With that fixture installed on an isolated macOS host:

```sh
cmake -S . -B build-ca -DHA_BUILD_NATIVE_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-ca --parallel
ctest --test-dir build-ca -R coreaudio_loopback --verbose
```

The test explicitly selects BlackHole 2ch, configures that fixture to 48 kHz,
and sends only synthetic data through it. It does not change default endpoints
or select a physical microphone/speaker. Run it only on a disposable fixture
host: it changes the virtual device's nominal sample rate. The executable has
an embedded microphone usage description and remains subject to normal macOS
permissions. The workflow does not modify privacy databases or disable OS
security. Missing devices, denied permission and failed sample checks are test
failures, not successful skips. CI artifacts retain driver version, platform
and verbose test logs; setting up the workflow alone is not qualification.

The workflow additionally runs ASan/UBSan with an explicitly instrumented profile
that records reported transport discontinuities. Strict Release continuity is
checked separately. See [CoreAudio results](coreaudio-qualification.md), including
the retained earlier failures. The corresponding Windows plan is in
[WASAPI paired-endpoint design](wasapi-loopback-design.md); it has not yet been
executed on a provisioned Windows audio fixture.

The [Windows qualification harness](wasapi-qualification.md) is now implemented.
`tools/check_windows_builds.ps1` runs the device-free native static/shared
Debug/Release and installed-consumer matrix. Actual audio tests require explicit
endpoint keys; the paired runner requires approved signed fixture manifests.
ClangCL x64 DLL-CRT builds accept `HA_SANITIZER=address` (ASan only), with matching
compiler runtime libraries. The local LLVM 19 runtime fails before test entry on
Windows build 26300; see the retained [results](native-windows-results.md).

## Opt-in physical CoreAudio qualification

`HA_BUILD_NATIVE_TESTS=ON` also builds `hail_coreaudio_physical` on macOS.
It is intentionally **not registered with CTest**: selecting real devices is an
explicit action. The test host links AVFoundation for normal microphone
permission and embeds its own purpose string; Hail's public library is unchanged.
No microphone PCM is saved or uploaded. Playback uses a quiet synthetic signal
and ends at each trial teardown. Enumeration alone opens no audio stream:

```sh
cmake -S . -B build-ca-physical -DCMAKE_BUILD_TYPE=Release -DHA_BUILD_NATIVE_TESTS=ON
cmake --build build-ca-physical --parallel
codesign --force --sign - build-ca-physical/hail_coreaudio_physical
./build-ca-physical/hail_coreaudio_physical
```

Choose exact enumerated names and the devices' existing nominal rate. Arguments
are capture name (or `-`), playback name (or `-`), rate, channels, PCM (`1` S16,
`2` F32), nominal seconds. This command captures in memory and plays sound:

```sh
python3 tools/run_coreaudio_physical.py \
  --test build-ca-physical/hail_coreaudio_physical \
  --capture 'MacBook Pro Microphone' --playback 'MacBook Pro Speakers' \
  --rate 48000 --channels 1 --pcm 2 --seconds 600 \
  --output /tmp/hail-physical-sustained.json
```

Use only devices and playback levels authorized by their owner. No defaults,
volume, device rates, privacy database or power settings are changed. Exact
names must be unambiguous; selection resolves within the same context. The
runner's `--matrix` requests 16/24/44.1/48 kHz, mono/stereo, S16/F32 once each.
Unsupported requests retain their nonzero trial exit and explicit unsupported
classification; they are not passing trials. The runner continues the matrix
and returns nonzero for unexpected failures. Its output includes raw logs,
source/binary hashes, configuration and platform commands, never PCM. Retain
all outputs, including failures, rather than retrying for a clean-looking run.

The test warms up for one second, measures a nominal interval with primed
playback, then flushes, rejects stale writes, stops/restarts and retires active
units. A capture reader runs independently from blocking control progress.
Eight additional active open/close cycles exercise reclamation within one
context. No timeout frees live Hail/native references; a failed assertion ends
the process and is a failed trial. The microphone need not contain the playback
signal, so nonzero capture is not a known-PCM loopback assertion. Host timestamp
age is not calibrated hardware latency. See [local results](native-macos-results.md)
for observed limits, toolchain failures and outstanding qualification cases.

For callback and native flush subcall measurements, configure a separate build
with `-DHA_COREAUDIO_DIAGNOSTICS=ON`. This defaults off and adds fixed private
trace storage included in planning, with no public API. The physical executable
copies histograms only after native retirement. `--silent --load-workers 4` on
the runner tests the render path with digital silence plus four SHA256 processes;
worker processes are stopped in a finally block. Instrumented callback body
measurements exclude final snapshot publication/unwind and retain transition
intervals. Keep them separate from the default uninstrumented profile.

Native registry lifecycle testing requires an explicit output UID and rate:

```sh
./build-ca-physical/hail_coreaudio_registry BuiltInSpeakerDevice 48000
./build-ca-physical/hail_coreaudio_registry BuiltInSpeakerDevice 48000 --defaults
```

The first command creates/removes a **private owned aggregate** around that
output. The second briefly publishes it and changes the output default to test
pinning and future-open behavior, then restores the original default and removes
only the owned fixture. Run when the temporary routing change is appropriate.
Ordinary failure cleanup restores/destroys the fixture too. It is not a physical
USB unplug test. No capture or nonzero output is used.

A static native-test build also provides `hail_coreaudio_faults`:

```sh
codesign --force --sign - build-ca-physical/hail_coreaudio_faults
./build-ca-physical/hail_coreaudio_faults BuiltInMicrophoneDevice BuiltInSpeakerDevice
```

It uses explicitly selected real units with silence, injecting SDK start/stop
and listener-removal failures in a source-private test copy of the backend.
It proves rollback state/retry and retirement behavior, separately from actual
OS denial or spontaneous device failures. It is not built against shared Hail's
hidden private symbols and is never auto-run by CTest.

For an actual permission-denial trial, use the dedicated signed test app through
LaunchServices, not a direct executable child inheriting an authorized terminal:

```sh
python3 tools/run_coreaudio_permission.py \
  --test build-ca-physical/hail_coreaudio_permission \
  --input 'MacBook Pro Microphone' --output /tmp/hail-permission-denied.json
```

Choose **Don't Allow** if macOS prompts for “Hail permission denied test.” The
runner uses normal authorization, a dedicated bundle identity and App Sandbox
without audio-input entitlement. It never resets/modifies TCC. An authorized
identity, unanswered prompt, timeout or absent input is **not a passing denial
case**. Timeout cancels the owned process and retains a failed/unqualified result;
it never forces Hail to free live references. The native result must explicitly
report permission denial. See [retained attempts and gaps](native-macos-followup.md).

CoreAudio's explicit denial guard additionally uses one private Objective-C
source and system AVFoundation/Foundation. The selected macOS backend therefore
needs an Objective-C-capable compiler; portable and other native builds do not.
No Objective-C type enters installed headers. Static C/C++ consumers receive the
framework dependencies through the exported target/pkg-config configuration.
The guard queries existing authorization only; hosts still own permission prompts.

The hosted BlackHole job explicitly sets `HA_TEST_COREAUDIO_PERIOD=2048` for a
conservative virtual-fixture functional profile. Application quantum remains
256, and strict Release sample/order/generation/clock checks are unchanged.
This is not low-latency qualification at a 512-frame native period: retained
512-frame hosted failures remain failures. The executable leaves the existing
period unchanged unless that environment variable is supplied. It requires an
idle fixture, avoids redundant nominal-rate writes, and waits for the native
property notification when changing rate or period. Both settings affect only
the explicitly selected BlackHole fixture; use an isolated device.

`hail_coreaudio_permission_cost [warm_iterations]` measures the read-only SDK
query without opening audio or requesting access. The source is
`tools/probes/coreaudio_permission_cost.m`; when compiling it manually, link
AVFoundation, Foundation and CoreAudio. Native tests also register the static-only
`coreaudio_permission_worker` CTest, which uses barriers and no audio opens to
verify that a blocked SDK query cannot block poll, that denial reaches capture,
and that destruction joins the worker. It runs in the normal, ASan/UBSan and
supported TSan configurations; it does not establish instrumented driver coverage.
