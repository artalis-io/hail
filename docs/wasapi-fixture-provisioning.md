# Staged signed paired fixture, 2026-11-18

VB-CABLE package 46 is now installed and the required reboot was verified.
The [paired execution report](native-windows-paired-results.md) records successful
driver/endpoint validation but four failed strict PCM cases. Paired qualification
remains open. The original staging/provisioning procedure below is retained;
the Shure physical endpoint tests do not replace the paired oracle.

Official [download and installation instructions](https://vb-audio.com/Cable/)
require extracting the complete package, running the standalone vendor setup as
administrator, and rebooting after installation (and after uninstallation).
The [vendor licensing page](https://vb-audio.com/Services/licensing.htm) permits
testing the workflow first and requires a paid license for professional use.
The bundled readme prohibits integrating its package into another installation
procedure without author agreement. Hail neither bundles nor installs it.
No license purchase or unattended deployment permission has been assumed.

## Candidate verified locally

- Package: `VBCABLE_Driver_Pack46.zip`, official download:
  `https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack46.zip`.
- Package SHA-256: `BC44E8974191A5A2427D6C979E2DAB08DBB09925F3F435DFA2F0BA3E45B6EB90`.
- Windows x64 driver: version `3.5.2.0`, INF date `2026-10-31`.
- Setup, SYS and CAT Authenticode signatures are Valid. Kernel-policy/catalog
  verification passed with SignTool, zero warnings/errors. The catalog signer is
  Microsoft Windows Hardware Compatibility Publisher.
- Local staged installer:
  `build-wasapi-fixture/pack46/VBCABLE_Setup_x64.exe`.

[Candidate hashes/signers](evidence/wasapi-fixture-candidate-2026-11-18/candidate.json)
and [kernel verification](evidence/wasapi-fixture-candidate-2026-11-18/signature-verification.zip)
are committed; vendor binaries remain outside the repository.

## Owner provisioning checkpoint

1. Confirm this standalone-driver evaluation and the applicable license terms.
   If an approved preprovisioned fixture already exists, use its records instead.
2. Record current default capture/playback devices and microphone settings.
   Close other audio work and save work before the required reboot.
3. Run the staged vendor installer as administrator using normal Windows consent.
   Do not bypass signing, Secure Boot, privacy or memory-integrity controls. If
   Windows rejects installation, preserve the error and stop provisioning.
4. Reboot normally. Confirm the original physical defaults remain selected;
   restore them through normal Windows sound settings if installation changed them.
5. Return to Hail and enumerate exact cable endpoint keys/PnP ancestry. Complete
   the approved fixture manifest with installed driver/package identities,
   reservation, engine formats, unity levels and disabled effects. Do not infer
   a transparent path from endpoint names or a successful installer.
6. Execute the strict known-PCM pair tests and preserve failures, followed by
   sustained/fault trials. Do not label physical-device or hosted CI results as
   a passing paired oracle. Conversion profiles require their own numerical oracle.

Until this checkpoint is completed, paired qualification remains open. The
completed Shure work can be reviewed/merged with that limitation documented;
merge does not assert fixture provisioning or complete production readiness.
