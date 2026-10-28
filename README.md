# LMZ Signal 2M 0.4.0 for macOS

Monophonic MIDI-triggered VST3 instrument by LMZ. This revision brings the Windows0.4.0 features to macOS: independent tempo-sync buttons for LFO and delay, Pre/Post FX tails, fourteen stable automated controls, the updated panel, build-version display and remote donation links.

The manual GitHub Actions workflow builds Universal arm64+x86_64, runs the ten native technical suites on each runner, and produces Signal2M-v0.4.0-macOS-Universal.zip. The archive contains one top-level Signal2M.vst3 bundle. Legal notices and build provenance are embedded inside Resources before signing. The recipe sets SIGNAL2M_SHOW_BETA=OFF and does not change either update sheet.

Build on a Mac with Apple Command Line Tools, Git and native Python3:

```sh
bash tools/build_macos.sh
```

The pinned JUCE version is7.0.12 at4f43011b96eb0636104cb3e433894cda98243626. Mac updates use gid640439888; shared donation links use Don!A1/A2, gid1525027926. No network work is performed by audio processing.

The default signature is ad-hoc, without Developer ID or Apple notarization. Deployment targets are Intel macOS10.15 and Apple Silicon macOS11.0; execution on the minimum OS and interactive DAW acceptance are separate from CI on macOS15. Technical reports are retained in workflow diagnostics. Source documentation stays in this repository and is not placed beside the delivered VST3.

Third-party components retain their original terms; see THIRD_PARTY_NOTICES_MACOS.md and the embedded Legal resources. The previous0.3.1 native build history remains in the development records.
