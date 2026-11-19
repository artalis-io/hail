# Hail: bounded, timestamped audio for C

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/artalis-io/hail/actions/workflows/ci.yml/badge.svg)](https://github.com/artalis-io/hail/actions/workflows/ci.yml)
[![C11](https://img.shields.io/badge/language-C11-22d3a8)](include/hail.h)
[![Version 0.1](https://img.shields.io/badge/version-0.1-provisional)](docs/api.md)

Hail is a portable **capture, playback and full-duplex PCM transport** written in
C11. Bounded queues carry samples and timing metadata between native audio
devices and application threads. Capture continues while playback is interrupted;
generations keep delayed writes from reviving cancelled audio.

Transport, execution ownership and platform implementation stay separate:

```text
             application / optional Hull binding
             speech · DSP · recording · sensing
                            |
                  timestamped PCM + status
                            |
              +-------------+-------------+
              |                           |
         capture queue              playback queue
         drop newest                silence on underrun
              |                           |
              |                    render reference
              +-------------+-------------+
                            |
                  private backend contract
                 /                        \
       execution ownership            platform resources
       serialized external drive      CoreAudio · WASAPI
       concurrent gates/snapshots      PipeWire · ALSA · virtual
```

Execution ownership is explicit rather than inferred from a platform name.
Backend-specific clocks, fault controls and scratch stay inside their adapter.
Structural checks enforce the portable boundary; source and failure-path audits
are recorded in [the axis audit](docs/axis-audit.md) and [C audit](docs/c-audit-2026-11-17.md).

**One Hail allocation per context and stream. No steady-state Hail allocations.**
Native SDK allocations are outside that budget. Public names use `ha_` and `HA_`;
streams and contexts are opaque. The version 0.1 ABI remains provisional.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/hail_virtual_example
```

The deterministic example needs no audio hardware. It uses a caller-owned arena
and exercises duplex PCM, interruption, stale writes and device loss.
CoreAudio is enabled by default on macOS, WASAPI on Windows; the default tests
do not open native audio streams.

```sh
# Linux native backends; requires ALSA and PipeWire development packages
cmake -S . -B build-linux -DHA_BACKEND_ALSA=ON -DHA_BACKEND_PIPEWIRE=ON

# Shared library
cmake -S . -B build-shared -DHA_BUILD_SHARED=ON

# Freestanding allocation: supply an allocator; native backends are excluded
cmake -S . -B build-arena -DHA_FREESTANDING=ON

# ASan + UBSan with a supported GCC/Clang toolchain
cmake -S . -B build-asan -DHA_SANITIZER=address -DCMAKE_BUILD_TYPE=Debug

# Native Windows: use ClangCL for C11 atomics
cmake -S . -B build-win -T ClangCL
cmake --build build-win --config Release --parallel
ctest --test-dir build-win -C Release --output-on-failure
```

See [the build guide](docs/build.md) for dependencies, installation, CMake and
pkg-config consumers, sanitizers and explicitly selected native tests.

## Ordinary PCM, explicit control

An opened, running duplex stream exposes bounded copy operations. Each block
carries its sample position, clock domain/epoch and available timing facts.

```c
#include <hail.h>

/* Function-body excerpt. s is an opened, running mono S16 duplex stream.
 * This example runs on its serialized control owner; dedicated PCM roles
 * can run concurrently. */
ha_stream_status status = HA_INIT(ha_stream_status);
ha_block_info block = HA_INIT(ha_block_info);
int16_t input[256], output[256] = {0};
uint32_t captured = 0, accepted = 0;

ha_result result = ha_stream_get_status(s, &status);
if (result == HA_OK) {
    result = ha_stream_write(s, output, 256,
                             status.playback_generation, &accepted);
    /* Handle partial acceptance, WOULD_BLOCK, BUSY and terminal state. */
}
result = ha_stream_read(s, input, 256, &captured, &block);
/* Consume captured frames when result is HA_OK; an empty queue returns
 * HA_WOULD_BLOCK. PCM format must match the negotiated stream format. */
```

This illustrates the transfer API, not construction or teardown. The complete
[virtual example](examples/virtual.c) demonstrates those lifecycle steps; the
[native example](examples/native.c) enumerates real endpoints without opening a
microphone. Start, stop, flush and close complete through control polling.
Applications must quiesce their data roles before final destruction.

## Contracts that matter

- **Independent duplex legs.** Capture/render rates, periods and clocks need not match.
- **Bounded flow.** Capture drops newest when full; playback fills missing frames with silence. Losses remain observable.
- **Playback generations.** Flush invalidates old writes. Native render-only reset preserves capture where supported.
- **Honest time.** Clock domains, epochs, validity and uncertainty accompany PCM. Unknown timing stays unknown.
- **Render reference.** Optional selected playback PCM supports external processing; it is not proof of audible DAC output.
- **Safe retirement.** Cancellation does not free storage. Native workers/callbacks must retire before reclamation, including failed opens.
- **Exact application PCM.** Native-endian S16/F32, interleaved 1–32 channels, 8–192 kHz; named mono/stereo or indexed discrete channels. Native support and conversion facts are reported separately.
- **Allocator control.** Exact Hail memory plans and custom allocators support fixed arenas; the core has no mandatory OS, DSP or runtime dependency.

## Platforms and evidence

The [Gate E assessment](docs/gate-e.md) and independent
[public DSP consumer](examples/dsp-consumer/README.md) cover DSP interoperability.
Gate E remains partial pending physical timing, alignment and interruption evidence.

Implementation, execution and hardware qualification are distinct support levels.

| Target | Implementation and current evidence | Remaining qualification |
| --- | --- | --- |
| macOS / CoreAudio | Native static/shared; scoped M1 Max physical duplex; arm64 macOS 15/26 strict BlackHole matrix and sanitizers | Broader devices/transports and calibrated physical latency; hosted profile uses a 2,048-frame native period |
| Windows / WASAPI | Shared-mode backend; ClangCL builds/consumers, Shure streaming, USB loss/reopen, ASan and SDK buffer-boundary diagnostics | [Exact paired transitions remain unqualified](docs/wasapi-marker-results.md); S16 conversion and calibrated physical timing remain open |
| Linux / PipeWire and ALSA | Native static/shared; isolated PipeWire loopback, ALSA null, fault tests and sanitizers | Real devices, including intended Raspberry Pi configurations |
| Freestanding / virtual | Fixed-arena example, deterministic conformance and backend-free compile | No FreeRTOS, UEFI or bare-metal audio adapter yet |
| Android / iOS | Future platform work | Dedicated native adapters and device qualification |

The audit-fix baseline passed **all 11 general CI jobs**, plus the complete
CoreAudio workflows on macOS 15 and 26. These are specific configurations, not a
blanket latency or production-readiness claim. See [verification](docs/verification.md),
[CI evidence](docs/evidence/audit-ci-2026-11-17.json), and the
[retained CoreAudio timing failures](docs/coreaudio-ci-regression.md).

## Fits into a larger stack

Hail supplies device I/O and timestamped PCM. Resampling, AEC, VAD, codecs,
networking, inference and conversation policy belong in external components.
The [duplex speech-service roadmap](docs/roadmap.md) describes the client adapter,
gateway and `herding-sparks` boundary; these services are not bundled with Hail.
An optional [Hull](https://gethull.dev) binding can mediate capabilities without moving native audio
servicing onto a language-runtime thread.

## Documentation

| Topic | Reference |
| --- | --- |
| API, threading and ownership | [Public API](docs/api.md) |
| Design and independent responsibilities | [Architecture](docs/architecture.md) · [axis audit](docs/axis-audit.md) |
| Writing a backend | [Private contract](docs/backend-authoring.md) · [native limitations](docs/native-backends.md) |
| Qualification | [Verification](docs/verification.md) · [macOS](docs/native-macos-goal.md) · [Windows](docs/native-windows-goal.md) |
| Loopback fixtures | [CoreAudio](docs/coreaudio-qualification.md) · [Linux](docs/loopback-qualification.md) · [WASAPI design](docs/wasapi-loopback-design.md) |
| Audit workflows | [C audit skill](.claude/skills/c-audit/SKILL.md) · [axis audit skill](.claude/skills/axis-audit/SKILL.md) |
| Ecosystem and future work | [Repository audit](docs/audit.md) · [roadmap](docs/roadmap.md) |

## Website

The [landing page](site/index.html) follows Keel's self-contained site pattern:
static HTML/CSS, local SVG assets, no runtime CDN or build-time package dependency.

```sh
make -C site check
make -C site build
make -C site serve
```

## License

MIT. Copyright (c) 2026 Artalis. See [LICENSE](LICENSE).
