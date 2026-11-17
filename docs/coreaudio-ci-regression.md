# CoreAudio CI regression investigation

This report preserves the distinction between a production control-path
regression, hosted native timing failures, and fixture configuration. It does
not reinterpret a failed strict test as a passing instrumented test.

## Retained failures

[Run 44119500727](https://github.com/artalis-io/hail/actions/runs/44119500727),
source `20791b9`, passed its initial strict 48 kHz stereo F32 process and one
repetition, then failed the next repetition on capture epoch 1 → 2 at logical
frame 2048. It never reached the format matrix or ASan step. The [raw job and
fixture logs](evidence/coreaudio-ci-regression/) are retained.

General CI run 44119500256 had a separate infrastructure failure: its macOS
arm64 jobs were cancelled before execution because hosted runners were not
acquired. Linux/Windows results were not erased or substituted for macOS.

A read-only AVFoundation probe on the local M1 Max/macOS 26.6.2 host measured
1,000 warm authorization queries: mean 16.281351 ms, minimum 15.164042 ms,
maximum 22.360958 ms; cold query 22.092375 ms. It opened no audio or permission
dialog. The original source and output are retained as
[permission-query-cost-source.m](evidence/coreaudio-ci-regression/permission-query-cost-source.m)
and [permission-query-cost.log](evidence/coreaudio-ci-regression/permission-query-cost.log).
The maintainable probe is [coreaudio_permission_cost.m](../tools/probes/coreaudio_permission_cost.m).
Query cost varies by process/host; the next hosted probe measured a much lower
0.746187 ms warm mean. Neither figure is a universal SDK latency bound.

The newly introduced synchronous query on **every** context poll was a real
regression: an application servicing PCM on that owner could be held inside an
unrelated authorization query. This explains a demonstrable producer-stalling
hazard; it does not, by itself, establish the cause of every driver clock reset.

## Backend and regression fix

Fresh probe/open/start checks remain synchronous and preserve known-denial
rejection. A single private worker is created lazily per context upon successful
capture open. It serializes SDK reads with fresh checks, publishes an atomic
result and waits 250 ms between reads. It sleeps without querying while no
capture streams remain. Poll consumes the result without waiting for that
worker. No authorization request/reset, native audio-callback SDK query or
public ABI change is introduced.

Context destruction signals and joins the worker before destroying its mutex,
condition variable and context storage. The worker never dereferences a stream;
stream count changes are mutex protected and callbacks never take that mutex.
Hail-owned state remains included in memory plans; the native thread stack and
SDK allocations remain outside the Hail allocator budget. Query and owner-poll
delay mean revocation observation is not instantaneous or hard bounded.

[The barrier-based worker test](../tests/coreaudio_permission_worker.c) opens no
audio. It holds an SDK query, completes 1,000 polls without calling/waiting for
that query, verifies publication of denial and replacement by a fresh query,
and proves worker retirement precedes synchronization-storage retirement.
Release, LLVM ASan/UBSan and LLVM TSan executions pass locally; this is not
instrumentation of Apple's frameworks/driver. The separate authorized physical
regression uses silent playback, verifies capture through flush/restart and
balanced repeated teardown, with zero startup/steady/transition underruns or
capture loss. Raw output and source hashes are retained.

## Hosted fixture investigation

The post-worker [run 44129533268](https://github.com/artalis-io/hail/actions/runs/44129533268),
source `3d5697e`, passed the strict baseline/repetitions and all tested 16/24/44.1
kHz mono/stereo S16/F32 cases. It then failed the 48 kHz mono S16 trial with an
observed raw position 51472 versus expected 50688, at logical frame 50688.
The test reported epoch 1 → 2 and stopped; no tolerance, event credit or retry
was used to turn that process into a pass. Later formats, shared native trials
and ASan in that run were not executed. Its 512-frame native period and full
failure logs remain in [the retained artifact](evidence/coreaudio-ci-regression/44129533268/).
This confirms the slow-query fix was not the entire explanation for the hosted
512-frame clock-discontinuity problem. The scheduling/driver cause is unproven.

The fixture now requires an idle explicitly selected BlackHole device. It does
not set a matching nominal rate again; actual rate changes wait for a native
property notification and readback before opening units. BlackHole's upstream
[0.7.1 source](https://github.com/ExistentialAudio/BlackHole/blob/v0.7.1/BlackHole/BlackHole.c)
already avoids requesting configuration changes for equal rates, so avoiding
that no-op write is fixture hygiene, not a claim to explain the observed gap.

The hosted job declares `HA_TEST_COREAUDIO_PERIOD=2048` as a conservative
**functional** virtual-device profile, leaving application quantum 256 and
capture/playback capacities unchanged. At 48 kHz this period is approximately
42.67 ms; at 16 kHz it is 128 ms. These are calculated periods, not measured
latencies. Native property notification/readback confirms the configured period.
No phase samples, order, generation, clock-continuity, loss or allocator checks
are relaxed. Any new gap still fails. No fallback period or retry-until-pass
logic is added. Retained 512-frame hosted failures remain unqualified at that
configuration; a larger-period pass cannot establish low-latency performance.
The separate physical 512-frame results remain scoped to the measured Mac/devices.

## Cross-platform runner availability

The general portable/consumer macOS jobs use the documented standard
`macos-15-intel` runner rather than contending for arm64 capacity. The native
BlackHole lane remains arm64. Its default image is pinned to `macos-15`, with
`macos-26` available as an explicit dispatch choice; the queued macOS 26 job was
retained and completed. This does not move native testing to Intel or infer an
OS-specific result from another OS. [GitHub's runner reference](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)
lists these architectures/labels. General [run 44129532648](https://github.com/artalis-io/hail/actions/runs/44129532648)
and [run 44130819066](https://github.com/artalis-io/hail/actions/runs/44130819066)
pass all eleven jobs, including both Intel macOS linkages, Linux sanitizers and
Windows builds/consumers. Windows native streaming remains unqualified.

## Final declared-period results

Both native arm64 profiles pass the complete matrix, strict repetitions and
separate ASan/UBSan stage:

| Profile | Source revision | Strict Release processes / verified frames | ASan/UBSan processes / verified frames |
| --- | --- | --- | --- |
| [macOS 26.7.2, 25G332](https://github.com/artalis-io/hail/actions/runs/44130819943) | `720962f085f3b51e1d93b6b5362d859cd5d35c07` | 40 / 3,711,744 | 3 / 364,288 |
| [macOS 15.8.2, 24H232](https://github.com/artalis-io/hail/actions/runs/44132651383) | `53472941b85acd248c6c53e5c20c16099888b2aa` | 40 / 3,638,016 | 3 / 363,776 |

Each OS executes all sixteen combinations of 16/24/44.1/48 kHz, mono/stereo,
S16/F32 for **each** linkage, plus eight baseline/repetition processes. Strict
Release has zero bad samples/order, capture frame gaps, capture queue loss,
reported clock resets or credited sample gaps. All six instrumented processes
also observed zero reported clock/sample gaps and no sanitizer diagnostics;
these observations remain separate from Release timing/correctness claims.

Raw [macOS 26 artifacts](evidence/coreaudio-ci-regression/44130819943/) and
[summary](evidence/coreaudio-ci-regression/44130819943-summary.json), and
[macOS 15 artifacts](evidence/coreaudio-ci-regression/44132651383/) and
[summary](evidence/coreaudio-ci-regression/44132651383-summary.json) preserve every
process's counters, fixture metadata, native period and poll measurements.

Release software flush acknowledgements ranged 31.950–137.171 ms on macOS 26
and 36.592–117.713 ms on macOS 15 across the tested formats. Cumulative playback
underruns ranged 0–16,128 and 0–22,528 frames respectively, including unprimed
startup/restart and transitions. They are not steady-state-only underrun rates;
no counts are discarded. Separate primed physical trials provide the nominal
steady-state observations. The 2,048-frame virtual profile is not proof of
512-frame hosted continuity, physical latency, an audible interruption bound or
hard real-time acceptance. The retained 512-frame failures remain failures.

