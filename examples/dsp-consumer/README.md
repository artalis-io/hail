# Independent public-API DSP consumer

This is a separate CMake project using an installed `Hail::hail`. It includes
no private header, production source file or fault seam. `consumer.c` includes
only `hail.h`; `main.c` supplies a deterministic device using the installed
public `hail/virtual.h`. It is executable interoperability evidence, not an AEC
algorithm or physical audio test. Hail itself does not depend on this consumer.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build --prefix "$PWD/installed"
cmake -S examples/dsp-consumer -B build-dsp -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PWD/installed"
cmake --build build-dsp --parallel
ctest --test-dir build-dsp --output-on-failure
```

On Windows use ClangCL as in the build guide and put the installed DLL directory
on PATH for a shared library. For an instrumented installed package use the same
compiler/runtime in the consumer. The fixture provides a caller allocator, so it
also works with a virtual freestanding Hail package. It requires the optional
virtual backend. Existing installed-consumer CI enables it through
`-DHA_CONSUMER_DSP=ON`; no source-tree include directory is injected.

Four main deterministic profiles cover nominal zero drift/equal rates, independent
positive/negative drift, unequal 16/44.1/48 kHz leg rates, mono/stereo, 4/32-channel
capture and 8-channel reference, variable blocks, capture/reference overflow,
render underrun zeros, output backpressure, native-required delayed cancellation,
stale writes, old reference generations, capture discontinuity, repeated restart,
default pinning, loss/reconnect, denied open, rate change by reopen and pending
retirement. The longest models 700 seconds; its wall runtime is not a latency
measurement. Even the zero-drift virtual profile conservatively reports separate
clock domains; actual shared-device clock reporting remains a native fact.

A further 32 short profiles exercise every capture count 1..32 with the reverse
playback counts 32..1. Each profile also cancels repeatedly while draining
references, queues distinct obsolete PCM before cancellation, checks capture
continuity and render discontinuities, and closes with pending flush while
retirement is held. Close reports cancellation separately from retirement;
application PCM gates reject further work until teardown finishes.

The processor selects microphone channel zero and applies exact gain 0.5 into
two bounded mono output slots. It keeps original capture time/rate, checks
continuity and estimates observed clock slope from valid correlations. All input
channels remain in copied capture storage. A real speech adapter must resample
externally when its requested rate differs. Render reference is drained
independently, with one bounded latest-block buffer, loss metadata, generation
and epoch tracking. Overload cannot grow storage or wait inside a device callback.

This mock discards obsolete selection records after reset to demonstrate
generation handling. A production AEC may retain earlier reference history for
the physical acoustic tail. No subtraction, acoustic alignment, echo rejection,
resampling quality, inference, codec or networking is claimed. Only the public
virtual device provides ground truth; physical qualification is separate.
