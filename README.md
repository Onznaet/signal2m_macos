# LMZ Signal 2M

Experimental monophonic VST3 synthesizer by LMZ, based on the Signal 2M instrument.

- MIDI note gate, retrigger and pitch transposition.
- VCO, LFO, internal Sync and feedback delay with Oscillation.
- Eleven stable automatable parameters and saved state.
- Illustrated front panel with LMZ logo, blue LEDs and a white upper indicator.
- macOS Universal target: Intel x86_64 and Apple Silicon arm64.

The macOS port is based on version **0.3.1**. Native macOS build and runtime validation are pending the first successful workflow run. The target minimum is macOS 10.15 for Intel and macOS 11 for Apple Silicon; minimum-OS runtime testing is separate.

## Build on macOS

Install Apple Xcode Command Line Tools, Git and native Python 3.9+ with pip, then run:

```bash
bash tools/build_macos.sh
```

The recipe downloads pinned dependencies into the project, builds both architectures, runs technical tests on the current native architecture, signs locally, and verifies the delivery ZIP. It does not install the plugin or launch a DAW.

## GitHub Actions

Manually run **macOS Universal (manual)** in the Actions tab with `upload_artifacts=true`. The workflow builds Universal packages on standard Intel and Apple Silicon macOS runners. It preserves diagnostics after failures and retains artifacts for seven days. Public standard runners are free; private repositories use the owner's included Actions allowance and billing settings.

A successful job produces `Signal2M-v0.3.1-macOS-Universal.zip`. Each job tests its native architecture; packages from different jobs are separate builds. First test distributions use an ad-hoc signature, with no Developer ID or Apple notarization. DAW and visual acceptance are performed separately.

## Documentation

- `docs/BUILD_MACOS_RU.md` — complete build instructions.
- `docs/INSTALL_MACOS_RU.txt` — install the whole VST3 bundle.
- `REVIEW_MACOS_REPORT.md` — performed checks and native checks still pending.
- `THIRD_PARTY_NOTICES_MACOS.md` — dependency notices and pinned versions.

Mac update checks use the dedicated Google Sheet tab `gid=640439888`; Windows retains `gid=0`. Checks run once when the editor first opens, and can be repeated manually. Audio processing never performs network requests.

Third-party dependencies retain their own licenses. See the notices and the original license files downloaded with the pinned dependencies.
