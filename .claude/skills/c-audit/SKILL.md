---
name: c-audit
description: Audit Hail C and Objective-C code for memory safety, bounded PCM validation, concurrency, native resource retirement, and build hardening. Use for a source safety audit or focused hardening review.
---

# Hail C audit

Adapted from Keel's `.claude/skills/c-audit/SKILL.md` at
`8f04a7b150fd8a619d76ef8085efbf380f7e1014`. Keeps its safety categories;
replaces HTTP and Keel allocator assumptions with Hail's audio contracts.

Target: `$ARGUMENTS`, default all `src/`, `include/`, relevant `tests/`,
`fuzz/`, CMake and CI. Audit only unless fixes are requested (`--fix` or ordinary
user authorization). Save dated results in `docs/c-audit-YYYY-MM-DD.md`.

Read `docs/api.md`, `docs/backend-authoring.md` and `docs/verification.md` to
establish the contract and distinguish implementation from qualification.

## Safety checks

| Area | Hail-specific checks |
|---|---|
| Memory | Checked add/multiply/alignment before allocation; exact plans and allocator sizes/alignment; fixed ring/scratch bounds; NULL/header/version validation; bounded strings and UTF-8 truncation. |
| Allocation | Hosted malloc/free belong in `hosted.c`; caller allocator owns Hail storage. SDK-owned strings/resources use the SDK's allocator/release rules, not Hail's allocator. Native SDK allocations are outside Hail's budget. |
| PCM | Frames are not bytes; validate format, channels, sample width, offsets, sizes, signed strides and native maxima before copying. Native buffer leases end through the SDK contract. Malformed geometry must not become silent conversion. |
| Queues | One producer/consumer per leg/role; release/acquire publication, modular index wrap, partial slots, bounded loss counters, capture drop-newest, render silence on underrun. No blocking to wait for application PCM. |
| Concurrency | Audio gates protect audio-owned state; snapshots transfer counters. Notifications publish bounded mailboxes/atomics, never mutate control state. Check error payload publication and cross-leg independence. |
| Lifecycle | Stop/flush/retire failures cannot acknowledge success. Cancellation and closed gates do not prove worker/callback retirement. Check join/wait/unregister results before reclaiming storage, including failed open/init. Retrying pending/error cleanup must preserve ownership and be idempotent. |
| Timing | Guard timestamp/frame arithmetic, unknown time, epoch overflow, partial-read interpolation, independent clocks. Callback time is not automatically capture/presentation time. |
| Interruption | Playback generations reject stale writes; consumer cutoff owns discard; render-only flush preserves capture. Render reference means selected PCM, not guaranteed DAC output. |
| Resources/errors | Pair native acquisition with release on every partial path. Normalize native failures without interpreting negative errors as lengths. Retained failed-open allocations remain bounded/accounted until retirement. |
| Dead code/build | Strict unused/conversion/shadow/format warnings; hosted stack protection when supported; freestanding exclusions; exact shared exports; no SDK dependencies in portable code. |

Scan with `rg` for unsafe copying/parsing, allocation arithmetic, ignored I/O
and retirement results, disabled code and platform dependencies. Follow concrete
control flow before assigning severity. Do not replace bounded `memcpy` or
SDK-owned `free` merely because it matches a search. Do not introduce generic
SAFE_FREE/CLAMP macros or mechanically substitute strncpy/calloc for a checked
allocator contract.

## Validation and output

Use existing builds or isolated build directories. Run relevant portable tests
and ASan/UBSan/TSan where supported, then affected native fault tests. Prefer
deterministic source-private SDK seams over sleeps or physical devices for error
paths. Device testing requires available hardware and existing user permission;
stop capture/playback immediately after each test. Builds and docs need no audio.

Examples (configure backends/toolchains for the host):

```sh
cmake -S . -B build-audit -DCMAKE_BUILD_TYPE=Debug
cmake --build build-audit --parallel
ctest --test-dir build-audit --output-on-failure
python3 tools/check_contracts.py build-audit/libhail.a
```

With native tests enabled, select device-free tests explicitly or exclude
`coreaudio_loopback`; do not accidentally treat a CTest run as permission to use
physical devices. A virtual test or cross-compile does not qualify native I/O.

Report revision, scope, severity totals and findings table: ID, severity,
file:line/symbol, current code, failure scenario, smallest fix. Distinguish
demonstrated defects, source-confirmed error paths and untested hypotheses.
Record exact checks/results and platform limitations. In fix mode preserve the
original audit, append resolution evidence, add meaningful regressions and rerun
the affected checks. Avoid unrelated cleanup and public API changes.
