# Hail

Hail is a standalone C library for bounded, timestamped audio capture,
playback, and full-duplex operation. Public identifiers use `ha_` and `HA_`.

**Status: Gate D native implementations, version 0.1.** CoreAudio, shared WASAPI,
PipeWire and ALSA now accompany the portable core and deterministic virtual
backend. Linux software-device duplex/interruption tests pass; macOS and Windows
backends compile and link. Physical-device qualification and latency measurements
remain outstanding. The ABI remains provisional.

- [Repository and audio implementation audit](docs/audit.md)
- [Architecture and contracts proposal](docs/architecture.md)
- [Verification plan and engineering gates](docs/verification.md)
- [Public API and ownership reference](docs/api.md)
- [Build and integration guide](docs/build.md)
- [Native backend contracts and limitations](docs/native-backends.md)
- [Backend authoring boundary](docs/backend-authoring.md)
- [Reproducible namespace evidence](docs/evidence/namespace.json)

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
