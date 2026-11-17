# Device-free fuzzing

These four libFuzzer targets exercise Hail's portable implementation. They never
open a microphone or a playback device. The static library and harnesses both
have coverage instrumentation, AddressSanitizer and UndefinedBehaviorSanitizer.
Checks use `CHECK`, which remains active with `NDEBUG`.

| Target | Inputs | Correctness checks |
| --- | --- | --- |
| `config` | Context/stream geometry, record headers, formats, negotiation, memory limits and allocation failure | Exact plan/accounting on success, matching validation failures, bounded allocations and complete cleanup |
| `lifecycle` | Up to 256 byte-selected operations: open/reopen, start, stop, poll, read/write/reference, flush, virtual steps, faults and close | State/result contracts, bounded transfers, playback generation monotonicity, stale writes, sticky terminal errors, no steady-state allocation and no leaked storage |
| `queue` | Slot/quantum/frame geometry, pushes, partial reads, resets, PCM and metadata | Independent slot FIFO model checks PCM order, counts, untouched output suffixes, generations/epochs, rational host-time interpolation, overflow validity and 32-bit cursor wraparound |
| `faults` | Scripted adapter failures at open/start/stop/flush, asynchronous device loss, delayed start and held roles/retirement | Failed-open storage remains charged, retirement retains storage, first terminal error/native payload stays paired, flush rejects stale PCM and preserves capture, cleanup happens once |

`faults` uses a source-private adapter seam, with independently selected serialized
or concurrent execution. Its single-threaded schedule pauses an admitted native
role across control progress, and releases application-role gates before or after
the adapter acknowledges retirement. These are controlled interleavings at gate
boundaries, not exhaustive instruction-level scheduling or native SDK coverage.
Actual threaded races remain covered separately by the existing TSan conformance
and handoff tests. The queue model tests sequential SPSC behavior; it does not
establish the correctness of atomic memory ordering.

The allocator tracks live blocks, sizes and alignments, rejects double/unknown
frees, and caps each allocation at 1 MiB. Inputs are at most 1,024 bytes, lifecycle
and queue processing is limited to 256 operations, and delays are bounded. Every
iteration cleans up before returning. No harness deliberately dereferences an
invalid pointer or calls through a freed handle.

## Build and run

Use Clang with a libFuzzer runtime (on macOS, Homebrew LLVM provides one):

```sh
cmake -S . -B build-fuzz -DCMAKE_C_COMPILER=clang \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHA_BUILD_FUZZER=ON \
  -DHA_BUILD_TESTS=OFF -DHA_BUILD_EXAMPLES=OFF -DHA_SANITIZER=address \
  -DHA_BACKEND_COREAUDIO=OFF -DHA_BACKEND_WASAPI=OFF \
  -DHA_BACKEND_ALSA=OFF -DHA_BACKEND_PIPEWIRE=OFF
cmake --build build-fuzz --parallel
python3 tools/run_fuzz.py --runs 100000
```

The runner generates named seeds before mutation, including complete duplex
lifecycles, delayed operations, hold/release, all failure phases in both execution
modes, maximum quantum, allocation failures and wraparound/partial-read queues.
It preserves the evolving corpus, logs, failing inputs and a JSON result summary
under `build-fuzz/results`. Each target has a wall-clock timeout; any failed or
timed-out target fails the command, while the other targets still run. Reusing
that directory retains discovered corpus entries; use a new `--output-dir` for a
fresh campaign. Individual targets can also run directly with libFuzzer flags.

Replay a saved input with the matching target and halt on the first UBSan error:

```sh
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ./build-fuzz/hail_fuzz_lifecycle build-fuzz/results/lifecycle/artifacts/crash-HASH
```

Keep the exact input and source/toolchain revision when investigating. Minimize
confirmed findings with libFuzzer's `-minimize_crash=1`, then retain a named seed
and a focused deterministic regression test for a production defect. A harness
assertion can also expose an incorrect oracle; establish which contract failed
before changing production code.

CI runs 30,000 inputs per target on pushes and pull requests; weekly campaigns
run 300,000 per target. Logs, corpus, artifacts, revision and CMake/toolchain
metadata are uploaded even on failure. A finite clean campaign is evidence for
those inputs, not a proof of memory safety or hardware qualification.
