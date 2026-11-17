# Hail

Hail is a standalone C library for bounded, timestamped audio capture,
playback, and full-duplex operation. Public identifiers use `ha_` and `HA_`.

**Status: Gate D native implementations, version 0.1.** CoreAudio, shared WASAPI,
PipeWire and ALSA now accompany the portable core and deterministic virtual
backend. Native platform builds and selected isolated loopback profiles have
execution evidence. Built-in macOS physical duplex has scoped qualification;
other physical targets and calibrated hardware latency remain outstanding.
See the reports for retained failures and exact configurations. The ABI remains provisional.

- [Repository and audio implementation audit](docs/audit.md)
- [C safety audit and fixes](docs/c-audit-2026-11-17.md)
- [Audio architecture axis audit](docs/axis-audit.md)
- [Architecture and contracts proposal](docs/architecture.md)
- [Verification plan and engineering gates](docs/verification.md)
- [Public API and ownership reference](docs/api.md)
- [Build and integration guide](docs/build.md)
- [Hail and duplex speech-service roadmap](docs/roadmap.md)
- [CoreAudio virtual-device qualification](docs/coreaudio-qualification.md)
- [WASAPI paired-endpoint test design](docs/wasapi-loopback-design.md)
- [Native macOS goal and handoff prompt](docs/native-macos-goal.md)
- [Native Windows goal and handoff prompt](docs/native-windows-goal.md)
- [Native software-loopback qualification](docs/loopback-qualification.md)
- [Native backend contracts and limitations](docs/native-backends.md)
- [Backend authoring boundary](docs/backend-authoring.md)
- [Reproducible namespace evidence](docs/evidence/namespace.json)

Repository audit workflows are in [c-audit](.claude/skills/c-audit/SKILL.md)
and [axis-audit](.claude/skills/axis-audit/SKILL.md), adapted from Keel for Hail.

The proposal keeps DSP, codecs, speech engines, inference, networking, recording
persistence, and application policy outside Hail. Hull integration is a future
external adapter, not a core dependency.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/hail_virtual_example
```

No audio hardware or external dependency is required for the virtual tests.
The example uses a fixed caller-owned arena and demonstrates duplex capture,
PCM playback, interruption, stale-write rejection and device loss.
