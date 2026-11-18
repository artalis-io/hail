# VB-CABLE installation/reboot checkpoint

The owner authorized proceeding with the staged standalone VB-CABLE evaluation
on 2026-11-18. Package 46, driver 3.5.2.0, has the verified hashes/signatures in
[candidate evidence](evidence/wasapi-fixture-candidate-2026-11-18/candidate.json).
No license purchase, redistribution or unattended driver-install integration is
authorized or performed. Professional deployment still needs applicable licensing.

Current work branch: `test/wasapi-paired-fixture`, based on merged main `c1b2de4`.
The Shure qualification and real USB loss/reopen already passed. Its original
microphone gain/mute is saved in the
[pre-install checkpoint](evidence/wasapi-fixture-install-2026-11-18/checkpoint.json).
There are no running Hail audio trials at installation time.

## Normal Windows installation

The vendor installer is staged at:

```text
C:\Users\Mark\artalis.io\src\hail\build-wasapi-fixture\pack46\VBCABLE_Setup_x64.exe
```

It must use normal administrator consent. Approve the Windows UAC prompt and use
the vendor's Install Driver action. Do not bypass signing, Secure Boot, privacy
or memory-integrity controls. Save work before rebooting. The vendor requires a
reboot after installation and after uninstallation; visible endpoints alone are
not proof that provisioning/reboot completed.

## Resume after reboot

1. Return to this repository and read this checkpoint, `docs/wasapi-fixture-provisioning.md`,
   `docs/wasapi-loopback-design.md`, and applicable repository/audit instructions.
2. Inspect `git status`/branch and preserve unrelated work. Check the installed
   VB-CABLE PnP/driver state, version, INF, hashes and signing against the candidate.
   A launched/closed installer does not prove installation succeeded. Record any
   installer failure and do not weaken system security to work around it.
3. Compare OS `LastBootUpTime` with the pre-install checkpoint. Record the new boot
   and actual installed state before declaring the fixture ready.
4. Compare all six Windows default roles using
   `build-windows-validation/OFF-Release/hail_wasapi_endpoint_level.exe --defaults`.
   The original roles are recorded exactly in
   `docs/evidence/wasapi-fixture-install-2026-11-18/before-default-roles.jsonl`:
   console/multimedia playback and capture were the Shure; communications were
   MOTIV Mix Virtual Input (playback) and MOTIV Mix Virtual Output (capture).
   If installation changed these defaults, restore them through normal Windows
   sound controls before tests. Do not use an undocumented policy or registry hack.
5. Enumerate Hail's exact new cable endpoint keys/directions, verify PnP ancestry
   to the actual installed owner, and record an exclusive fixture reservation.
   Do not fall back to Shure, Steam, Oculus, MOTIV or default devices.
6. Configure only the approved cable fixture for the documented transparent profile
   (baseline 48 kHz stereo, unity endpoint/channel gains, no effects or physical
   monitoring). Confirm actual engine formats and routing; do not infer these
   from the installer, names or a successful enumeration. Complete and validate
   its real manifest; do not copy placeholder approvals or invented identities.
7. Execute known-nonzero PCM through both Hail legs for S16/F32, native flush and
   stale generation checks, stop/restart, starvation/overflow, balanced allocation,
   a sustained duplex trial and supported diagnostics. Preserve every failed
   run. Numerical conversion cases need predeclared independent oracles; do not
   relax exact tolerances or accept zeros merely to obtain a pass.
8. Update the support matrix/audits only to execution actually observed, commit
   and push according to repository policy. No microphone PCM may be persisted
   or uploaded. Physical latency remains uncalibrated and must not be claimed.

The current physical fixtures/results stay valid independently of this virtual
pair. The paired criterion remains pending until the actual known-PCM run passes.
