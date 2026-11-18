# VB-CABLE paired qualification, 2026-11-18

Follow-up: [independent stage isolation](wasapi-path-results.md) locates the
high-level change at render-loopback output and S16 noise in native capture.
A separately predeclared low-grid F32 profile passes the short/ASan trials after
a demonstrated harness boundary correction. The original results below remain
failed and retained; this follow-up does not certify original-profile or S16
exact transparency.

The installed fixture was exercised through both Hail WASAPI legs. **All four
48 kHz stereo strict PCM cases failed.** The signed-driver/endpoint validator
passed, but this configuration does not satisfy `transparent-s16-grid`.
The Windows qualification goal remains open. The successful Shure physical
results remain independent of this failure.

## Provisioning and scope

[Provisioning evidence](evidence/wasapi-paired-2026-11-18/provisioning.json)
records the new boot at `2026-11-18T10:10:20.5000000+01:00`, later than the saved
pre-install checkpoint. Installed VB-CABLE 3.5.2.0 uses `oem54.inf`; its actual
DriverStore SYS hash matches the approved package candidate and its installed
signature is Valid, with the Microsoft catalog signer recorded by Windows.

Both selected endpoints have authoritative PnP parent `ROOT\MEDIA\0001`, status
OK and problem code zero. A second device, `ROOT\MEDIA\0002`, is present with
problem code 10; neither selected endpoint belongs to it. It was not repaired,
removed or selected for testing.

The [manifest](evidence/wasapi-paired-2026-11-18/manifest-48000-2.json) reserves
only the active cable pair for this evaluation. Its approval fields refer to
the previously authorized standalone evaluation and current owner resumption;
they do not authorize professional deployment, package redistribution or an
unattended driver installer. The reservation is temporary and expires.

Both engine formats were 48 kHz stereo F32. Capture master/channel gains were
already unity and unmuted. Cable playback initially read `0.999998987`; the
fixture-only helper set master and all sixteen exposed channel gains to exact
unity and read them back. That cable setting remains at unity. No audio defaults
or microphone settings were changed. All six default roles match the saved
pre-install roles both before and after testing. The Shure was muted, gain
`0.197611526`, at both session readbacks; its older pre-install checkpoint was
`0.209999993`, so the difference predates these trials and is not claimed restored
by this session. No microphone stream was opened or microphone PCM retained.

The selected cable endpoints and the additional 16-channel render endpoint
had no active audio sessions at reservation time. Endpoint FxProperties were
empty and the installed INF has no APO/effect registration. Those facts support
the manifest's configuration assertions but do not prove a transparent Windows
engine/driver path. The known-PCM test is the acceptance check, and it failed.
No monitoring route was configured by this work; a complete independent proof
of all existing system routing was not obtained.

## Execution

The original executable/source hashes and system facts are in the retained
[fixture validation](evidence/wasapi-paired-2026-11-18/runs/fixtures/0/fixture-validation.json).
The qualification runner used its existing exact oracle, five-second measured
phases, both sample formats and both startup modes. Each trial also attempted
native flush, stale-generation rejection, stop/restart and deliberate faults.

| Strict 48 kHz stereo case | Result | Verified A / B / C frames | Bad counter |
| --- | --- | --- | --- |
| S16, primed | Failed `!bad` | 178308 / 26235 / 30629 | 276383 |
| S16, unprimed | Failed `!bad` | 177949 / 26956 / 30414 | 278288 |
| F32, primed | Failed minimum C frames | 289440 / 43503 / 7913 | 48939 |
| F32, unprimed | Failed minimum C frames | 288384 / 43360 / 7915 | 49451 |

All four short measured phases reported zero capture loss, playback underruns,
reference loss, capture metadata gaps and capture epoch changes. Flush phases
also reported zero capture gaps/epoch changes. These partial observations do not
override the numerical failures or establish complete lifecycle/allocation
qualification: the final acceptance assertions failed before balanced teardown
was reported. The bad counter includes sample, sequence and metadata checks;
it is not a count of distinct corrupted frames.

The runner started its 600-second trial after the short cases. It was explicitly
stopped once all four numerical failures were confirmed, rather than waiting
for the same failing profile. Its partial log and abort explanation are retained;
**there is no completed ten-minute paired result**. The runner's aggregate also
records fourteen missing rate/channel/PCM profiles: only the 48 kHz stereo
manifest was supplied. There was no automatic retry, tolerance change, alternate
oracle profile, cable disable/re-enable or sanitizer audio campaign.

## Bounded diagnostics and remaining work

The harness now prints at most the first eight sample/sequence failures without
changing acceptance. Two separately labeled one-second native diagnostic trials
used that build. They failed too:

- S16 reported `(-1, 0)`, `(0, -1)` and `(1, 0)` in S16 units during expected
  silence. This is consistent with conversion noise/dithering, but does not
  identify which Windows/driver stage produced it.
- F32 reported high-level values such as `(32212.875, -32213)` in S16 units;
  they violate the exact grid and channel-negation checks. Observed peak was
  approximately `0.984997`. Limiting or another transformation is a hypothesis,
  not an isolated root cause.
- Sequence failures occurred at startup/restart too. Residual cable buffering
  and the harness's treatment of transition boundaries still need isolation;
  these failures must not be silently credited as expected discontinuities.

Microsoft documents that the shared engine processes floating-point samples and
can convert application sample sizes; matching nominal rates therefore does
not alone establish transparency. See [device formats](https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-formats).
This documentation does not prove the cause of the observed alteration.

The diagnostic-only C change builds, and all nine static Release device-free
CTest cases pass, including the independent oracle negative controls. Initial
sandbox diagnostic attempts could enumerate/read levels but failed native client
initialization; both those logs and the subsequent authorized native logs are
retained separately. The first provisioning read looked in System32/drivers;
the installed INF instead uses DriverStore destination 13, whose actual binary
was then hashed and validated.

Next work is to isolate render, cable and capture transformations independently
and establish a fixture configuration that satisfies the predeclared exact
profile. A separately specified numerical-conversion profile would require its
own independent oracle before execution. Neither widening the current tolerances
nor accepting these samples would complete qualification. Physical latency and
audible-tail bounds remain unmeasured.

[Raw trials and aggregate](evidence/wasapi-paired-2026-11-18/runs/summary.json),
[diagnostic helper source](evidence/wasapi-paired-2026-11-18/inspect.cpp) and
[provisioning recorder](evidence/wasapi-paired-2026-11-18/provision-record.ps1)
are retained. Logs contain synthetic-cable measurements and bounded synthetic
sample diagnostics, not microphone audio.
