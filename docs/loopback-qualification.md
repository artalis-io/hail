# Native software-loopback qualification

The test sends known, nonzero F32 stereo PCM through Hail's native PipeWire
backend, into a private server's null sink, then captures its monitor through
Hail. It uses the OS/server execution path rather than Hail's virtual backend.
No physical audio device is included in the graph.

The [C test](../tests/pipewire_loopback.c), [runner](../tools/run_pipewire_loopback.py)
and [fixture configurations](../tests/fixtures/pipewire/server.conf) make this
repeatable. The runner creates a fresh private server for each trial, links only
the chosen graph ports, destroys/replaces the endpoint, joins its children and
removes temporary files. It never modifies the desktop audio server. The
[PipeWire configuration reference](https://docs.pipewire.org/page_man_pipewire_conf_5.html)
describes the context modules and objects used by the fixture.

## Assertions and measurement boundaries

Each input frame carries an exactly representable frame code on the left and
its negative on the right. Separate code ranges distinguish the playback
generations. Captured nonzero frames must match exactly and retain order.
Silence at startup/underrun is counted separately. A new-generation sample must
never be followed by an old-generation sample. Backend capture positions and
clock epoch must remain continuous while playback is flushed. The test also
checks stale-generation rejection, device-loss failure, fresh runtime IDs after
replacement/reopen and a balanced Hail allocator ledger after destruction.
A separate post-reopen phase deliberately withholds playback and delays capture
reads, requiring native underrun/loss counters and later block loss metadata.
Those injected faults are excluded from the nominal delivery distributions.

Two one-second phases surround a native-required flush. Monotonic timestamps
are collected at application write admission and capture reads; the resulting
distribution includes Hail queues, native/server processing and application
polling. It is measured software round-trip delivery, not a DAC/ADC estimate.
Request-to-observed-flush-completion likewise includes polling and says nothing
about an audible tail. The harness polls approximately every 0.5 ms; the OS may
delay it. No latency threshold is used as a conformance oracle.

CPU time is the test/client process's user plus system time over those phases,
including its native audio threads; it excludes the separate PipeWire server.
RSS is Linux ru_maxrss, a process high-water mark. Hail peak allocation bytes
and steady-state allocation counts come from caller allocator hooks and exclude
native library allocations, native thread stacks and the server. Callback
execution time, callback jitter and physical latency remain unmeasured.
The fixed-capacity test buffers and final sorting are outside audio callbacks.

## Measured baseline (2026-11-16)

[Raw release results](evidence/loopback-linux-release.json) contain ten fresh-server
trials, compiler/options, source/executable hashes and the host configuration.
The build uses GCC 13.3, shared Release (-O3 -DNDEBUG), PipeWire 1.6.8,
48 kHz stereo F32, 256-frame graph/application quantum, 16 capture slots and
four playback slots. The host is Linux x86-64 on an AMD Ryzen Threadripper 9980X.
Ambient host load and power policy were not controlled.

| Observation | Measured result |
| --- | --- |
| Known nonzero frames verified | 957,696 across ten nominal two-second trials; zero bad samples or sequence gaps |
| Producer admission to capture-read median | 26.48–26.51 ms across trial medians |
| Producer admission to capture-read p99 | 27.03–27.10 ms across trial p99s |
| Request to observed native flush acknowledgement | 0.523–0.696 ms across ten requests |
| Nominal capture loss / playback underruns | Zero in all ten measured phases |
| Client CPU time | 13.70–15.96 ms per approximately two-second measured trial |
| Process RSS high-water | 17,596 KiB (ru_maxrss; includes framework/test process state) |
| Peak Hail-requested allocation | 297,504 bytes; zero Hail allocations during nominal processing/flush |
| Recovery and forced starvation | All ten pass loss/replacement, explicit reopen, underrun silence/counters and capture-overflow metadata |

The forced-starvation phases happen after measurement and deliberately report
nonzero loss/underruns. They are correctness tests, not part of the zero-xrun
nominal result. These short trials are a baseline, not a long soak or latency
guarantee. Physical round trip, audible interruption tail, callback duration,
callback jitter, server CPU and behavior under controlled CPU stress remain open.

Three [additional CPU-contention trials](evidence/loopback-linux-cpu-contention.json)
pin the runner/server/client to one CPU alongside one busy process. All pass
sample/order, interruption, recovery and fault assertions. Two report zero
underruns; one reports 256 underrun frames. These counters include stream startup,
so the observation does not identify an exact missed-deadline cause. Known PCM
remains ordered and correct, with silence separately counted. This is a small
controlled scheduling experiment, not worst-case stress qualification.

## Dependency finding

[Recorded bisection results](evidence/pipewire-leak-bisection.json) retain the
unfiltered diagnostic output and reproducer hashes.

The [standalone context probe](../tools/probes/pipewire_context.c) reproduces the
previous 3525-byte/34-allocation LeakSanitizer finding without Hail code. A
[configuration bisection](../tools/probes/pipewire_leaks.py) narrows it to the
RT module's DBus fallback: removing that module or disabling both RTKit and
portal fallback is leak-clean on this host. PipeWire's
[module-rt source](https://raw.githubusercontent.com/PipeWire/pipewire/1.6.8/src/modules/module-rt.c)
confirms those fallback switches control DBus setup. This is evidence about the
path involved, not proof of the exact faulty allocation owner or a production
fix. Default desktop configuration remains an open dependency qualification
item. [Repeated standalone context construction](evidence/pipewire-context-repetition.json)
reported 3525 bytes for one context and 35250 bytes for ten in one process,
so the finding is not merely a single process-start allocation in that setup. Hail does not disable scheduling services or call process-wide DBus
shutdown to conceal it. No sanitizer suppression is used.

The isolated loopback intentionally loads neither the RT module nor a session
manager. Its ASan/UBSan/LSan and TSan passes therefore establish the tested graph
and Hail handoff behavior, not default-desktop leak freedom or RT scheduling.
