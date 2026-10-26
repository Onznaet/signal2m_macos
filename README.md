# LMZ Signal 2M

Experimental monophonic VST3 synthesizer by LMZ, based on the Signal 2M instrument.

- MIDI note gate, retrigger and pitch transposition.
- VCO, LFO, internal Sync and feedback delay with Oscillation.
- Eleven stable automatable parameters and saved state.
- Illustrated front panel with LMZ logo, blue LEDs and a white upper indicator.
- macOS Universal: Intel x86_64 and Apple Silicon arm64.

Version **0.3.1** has been built and tested natively on both architectures in [successful run5](https://github.com/Onznaet/signal2m_macos/actions/runs/40419330616), from commit c3dc36f91ade3c6e5194ffceccd5a009faeab2ae. Both jobs passed **9/9 CTest checks**, signature and ZIP verification, and loading the archived VST3 in an independent native host.

## Ready-to-test delivery

The primary delivery is Signal2M-v0.3.1-macOS-Universal.zip, **5,767,813 bytes**, available inside the Signal2M-macOS-native-arm64 artifact of run5, under dist-macos. Artifacts are retained for seven days.

```text
SHA-256: d916a9fdbc52578769a74ad1e58ee8f5faa7c57dc767548344f4e6ed28e9aedf
```

This Universal package includes both arm64 and x86_64 and was tested natively on arm64. The Intel job tested a separate Universal build of the same commit on x86_64; its archive has a different hash.

Close the DAW, extract the delivery, and copy the entire Signal2M.vst3 bundle to ~/Library/Audio/Plug-Ins/VST3. Rescan VST3 instruments in the DAW and select **LMZ Signal 2M**. The ready plugin does not require Xcode or Python.

The delivery has an **ad-hoc signature**, with no Developer ID or Apple notarization. The deployment targets are macOS 10.15 for Intel and macOS 11.0 for Apple Silicon. CI ran on macOS 15; minimum-OS runtime, DAW, editor and visual acceptance remain manual checks.

## Build on macOS

Install Apple Xcode Command Line Tools, Git and native Python 3.9+ with pip, then run:

```bash
bash tools/build_macos.sh
```

The recipe downloads pinned dependencies into the project, builds both architectures, runs technical tests on the current native architecture, signs locally, and verifies the delivery ZIP. It does not install the plugin or launch a DAW.

The manual macOS Universal (manual) workflow builds on standard Intel and Apple Silicon runners, preserves diagnostics after failures, and saves artifacts when upload_artifacts=true.

## Documentation

- MACOS_HANDOFF_RU.txt — delivery identity and manual installation.
- docs/BUILD_MACOS_RU.md — complete build instructions.
- docs/INSTALL_MACOS_RU.txt — install the whole VST3 bundle.
- REVIEW_MACOS_REPORT.md — current independent review in this repository.
- THIRD_PARTY_NOTICES_MACOS.md — dependency notices and pinned versions.

The review report inside the delivered ZIP remains the source snapshot at build time and may contain earlier pending statuses. The current review is maintained in the repository; the verified ZIP has not been rewritten.

Mac update checks use the dedicated Google Sheet tab gid=640439888; Windows retains gid=0. Checks run once when the editor first opens and can be repeated manually. Audio processing never performs network requests.

Third-party dependencies retain their own licenses. See the notices and original license files included with the delivery.
