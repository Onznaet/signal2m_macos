"""Create a UTF-8, checksummed SOURCE handoff for the first Mac build.

This is not a compiled VST3 delivery. It deliberately excludes local tools,
dependencies, build results, credentials, user presets and Windows installers.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import stat
import zipfile

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = (
    "CMakeLists.txt", "REVIEW_MACOS_REPORT.md",
    "THIRD_PARTY_NOTICES_MACOS.md", "MACOS_HANDOFF_RU.txt", "DEVELOPMENT_MACOS_STATE.md",
    "TECHNICAL_DECISIONS.md", "DSP_DECISIONS.md", "SPEC_SIGNAL_2M_V1.md",
    "tools/bootstrap_mac.py", "tools/build_macos.py", "tools/build_macos.sh",
    "tools/package_macos.py", "tools/package_macos_source.py", "tools/macos_release.py",
    "tools/juce7-clang23.patch", "tools/macos-loopback-test-Info.plist",
    "docs/BUILD_MACOS_RU.md", "docs/INSTALL_MACOS_RU.txt",
    ".github/workflows/macos.yml",
    "Tests/ReviewerMacVst3Checks.cpp", "Tests/ReviewerMacPackageChecks.py",
    "Tests/UpdateCheckerTests.cpp", "Tests/MacUpdateTransportTests.mm",
    "Tests/ReviewerMacUpdateFixture.cpp", "Tests/ReviewerMacUpdateLifecycle.cpp",
    "Tests/Fixtures/Signal2M-v0.1-state.bin",
    "Source/UpdateHttpTransportMac.mm",
)
ALLOWED_SUFFIXES = {".cpp", ".h", ".mm", ".py", ".svg", ".md", ".bin"}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> None:
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\(Signal2M VERSION (\d+\.\d+\.\d+)\b", cmake)
    if match is None:
        raise SystemExit("Cannot read the numeric project version.")
    version = match.group(1)
    paths = {ROOT / name for name in REQUIRED}
    for name in ("Source", "Tests", "assets"):
        for path in (ROOT / name).rglob("*"):
            if path.is_file() and path.suffix in ALLOWED_SUFFIXES:
                paths.add(path)
    contents: dict[str, bytes] = {}
    for path in sorted(paths):
        if not path.is_file() or path.is_symlink():
            raise SystemExit(f"Missing or linked required source file: {path}")
        relative = path.relative_to(ROOT).as_posix()
        pieces = PurePosixPath(relative).parts
        if any(part in {".git", ".codex", ".agents", ".aws", "__pycache__", "build", "dist", ".tools"} for part in pieces):
            raise SystemExit(f"Private or generated path in handoff: {relative}")
        data = path.read_bytes()
        if path.suffix != ".bin":
            data.decode("utf-8")
        if path.suffix in {".sh", ".command"} and b"\r" in data:
            raise SystemExit(f"Mac shell script must use LF line endings: {relative}")
        contents[relative] = data
    manifest = {
        "kind": "macOS-build-source-handoff", "product": "LMZ Signal 2M", "version": version,
        "contains_compiled_vst3": False, "native_macos_build": "required",
        "target_architectures": ["arm64", "x86_64"],
        "mac_update_gid": "640439888", "windows_update_gid": "0",
        "juce_commit": "4f43011b96eb0636104cb3e433894cda98243626",
        "build_command": "bash tools/build_macos.sh",
        "files": {name: digest(data) for name, data in contents.items()},
    }
    contents["SOURCE_MANIFEST.json"] = (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    output = ROOT / "dist-macos" / f"Signal2M-v{version}-macOS-SOURCE.zip"
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(".zip.tmp")
    with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(contents.items()):
            info = zipfile.ZipInfo(name)
            info.create_system = 3
            permissions = 0o755 if name.endswith((".sh", ".command")) else 0o644
            info.external_attr = (stat.S_IFREG | permissions) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)
    with zipfile.ZipFile(temporary) as archive:
        if archive.testzip() is not None or set(archive.namelist()) != set(contents):
            raise SystemExit("Source ZIP CRC/file-set check failed.")
        for name, data in contents.items():
            if archive.read(name) != data:
                raise SystemExit(f"Source ZIP byte mismatch: {name}")
        mode = archive.getinfo("tools/build_macos.sh").external_attr >> 16
        if not mode & stat.S_IXUSR:
            raise SystemExit("Mac entry script lost its executable permission.")
    temporary.replace(output)
    checksum = digest(output.read_bytes())
    output.with_suffix(".zip.sha256").write_text(f"{checksum}  {output.name}\n", encoding="utf-8", newline="\n")
    print(f"SOURCE handoff (not a compiled plugin): {output}")
    print(f"Files: {len(contents)}; bytes: {output.stat().st_size}; SHA256: {checksum}")


if __name__ == "__main__":
    main()
