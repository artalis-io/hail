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

The optional configuration/virtual-sequence fuzzer uses Clang libFuzzer:

```sh
cmake -S . -B build-fuzz -DCMAKE_C_COMPILER=clang -DHA_BUILD_TESTS=OFF \
  -DHA_BUILD_EXAMPLES=OFF -DHA_BUILD_FUZZER=ON -DHA_SANITIZER=address
cmake --build build-fuzz --parallel
./build-fuzz/hail_fuzz_config -runs=30000 -max_len=96 -seed=73
```

On this host Clang also needed the GCC installation selection shown above in
`CMAKE_C_FLAGS`, because libFuzzer links a C++ runtime. Normal C library builds
do not require a C++ runtime. Persist any future crashing input as a regression
fixture; do not mark a native backend covered by this portable target.

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
validation need target tooling in the respective platform gates. The sibling
audit remains incomplete for unavailable Sail and Hull binary artifacts. Static
archives may contain private `hail_*` helper definitions; only `ha_*` functions
listed in the export manifest belong to the public interface. Shared builds
hide all helpers. Source acquisition and builds make no network requests.
