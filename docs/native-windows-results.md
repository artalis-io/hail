# Native Windows implementation and evidence, 2026-11-18

**WASAPI streaming remains unqualified.** This pass implements the qualification
artifacts and fixes source-confirmed defects. No native capture/playback stream
was opened. Physical endpoint/level selection and an approved signed fixture
manifest are still required. The Shure MV7+ physical pair and MOTIV Mix virtual
endpoints enumerate, but their existence does not establish audio execution or
transparent paired routing.

Base: `dbfbc27bccbd002495197cc0423fdde2a04a33da`. Native host: Windows 11 Pro x64,
build 26300, AMD Ryzen 9 5950X, ClangCL 19.1.5, CMake 4.4.3, Windows SDK 10.0.26100.
AudioEndpointBuilder and Audiosrv were running. No defaults, gains, privacy,
security settings or drivers were changed. See the
[evidence directory](evidence/native-windows-2026-11-18) for logs, hashes and
configuration; [reproduction instructions](wasapi-qualification.md) describe the
implemented harness and its limitations.

| Check | Actual result |
| --- | --- |
| Native static Debug/Release | 9/9 CTests each, including packet/command/notification/retirement and independent PCM-oracle tests |
| Native shared Debug/Release | 5/5 CTests each; private source-inclusion fault seams remain static-only |
| Installed C and C++ consumers | 2/2 for each of the four configurations |
| PE/COFF contracts | Exact 27 public symbols for each static archive/shared DLL; portable source boundaries passed |
| Native endpoint enumeration | Passed in each configuration, 10 active endpoints; no streams opened |
| Clang static analysis | No diagnostics in portable core, native handoff, WASAPI adapter or native harness |
| PowerShell scripts | Parse checks passed; placeholder manifest rejected before endpoint access |
| ClangCL x64 ASan | Built with the matching compiler runtime; all eight then-existing CTests failed before execution in Windows interceptor initialization |
| Application Verifier | Available, not enabled: current shell is not elevated; no diagnostic pass claimed |
| Physical capture/playback/duplex | Not run; endpoint/level choice requested from owner |
| Signed paired cable and real disable/restore | Not run; no approved provisioned manifest/procedures supplied |
| Ten-minute sustained duplex, format matrix | Harness implemented; not run without authorized endpoints/fixture |
| Delivery, flush, CPU, memory measurements | Measurement code implemented; no native-stream measurements collected |

The ASan raw report contains `interception_win: unhandled instruction` before
test entry. This is a toolchain/runtime compatibility failure, not passing memory
coverage or a diagnosed Hail memory error. No interceptor suppression, runtime
substitution, privacy bypass or retry-until-pass was used. A supported
runtime/OS pairing remains necessary on this build 26300 host.

The focused [C audit](c-audit-2026-11-18.md) records fixes and regressions; the
[axis audit](axis-audit.md) records preserved ownership/separation. The original
goal's physical, sustained, fixture and diagnostics acceptance criteria remain
open. Do not raise the support matrix to qualified or production-ready based on
these builds and device-free tests.
