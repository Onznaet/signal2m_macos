#!/usr/bin/env python3
"""Verify, locally sign, and ZIP a complete macOS Universal Signal2M VST3."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import re
import shutil
import stat
import sys
import tempfile
import zipfile

import bootstrap_mac as bootstrap


ROOT = bootstrap.ROOT
VERSION = "0.3.1"
DELIVERY_NAME = f"Signal2M-v{VERSION}-macOS-Universal"
ARCHITECTURES = {"arm64", "x86_64"}
MINIMUM_VERSIONS = {"arm64": (11, 0, 0), "x86_64": (10, 15, 0)}
CLASS_IDS = {
    "Audio Module Class": "ABCDEF019182FAEB4C6D7A7A5367326D",
    "Component Controller Class": "ABCDEF011234ABCD4C6D7A7A5367326D",
    "Plugin Compatibility Class": "ABCDEF01C0DEF00D4C6D7A7A5367326D",
}
MACHO_MAGICS = {b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca", b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca",
                b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe"}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise bootstrap.BootstrapError(message)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def tolerant_module_json(text: str) -> dict:
    """JUCE 7's bundled Steinberg helper emits trailing JSON commas."""
    result: list[str] = []
    inside_string = False
    escaped = False
    for index, character in enumerate(text):
        if inside_string:
            result.append(character)
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                inside_string = False
        elif character == '"':
            inside_string = True
            result.append(character)
        elif character == ",":
            following = index + 1
            while following < len(text) and text[following].isspace():
                following += 1
            if following == len(text) or text[following] not in "]}":
                result.append(character)
        else:
            result.append(character)
    parsed = json.loads("".join(result))
    require(isinstance(parsed, dict), "moduleinfo.json must contain an object")
    return parsed


def verify_metadata(bundle: Path) -> Path:
    require(bundle.is_dir() and bundle.name == "Signal2M.vst3", "Expected a complete Signal2M.vst3 bundle")
    plist_path = bundle / "Contents/Info.plist"
    require(plist_path.is_file(), "Missing Contents/Info.plist")
    metadata = plistlib.loads(plist_path.read_bytes())
    expected = {
        "CFBundleIdentifier": "sound.lmz.signal2m", "CFBundleExecutable": "Signal2M",
        "CFBundleName": "Signal2M", "CFBundlePackageType": "BNDL",
        "CFBundleVersion": VERSION, "CFBundleShortVersionString": VERSION,
    }
    for key, value in expected.items():
        require(metadata.get(key) == value, f"Info.plist {key} must be {value!r}; got {metadata.get(key)!r}")
    executable = bundle / "Contents/MacOS/Signal2M"
    require(executable.is_file() and os.access(executable, os.X_OK), "Missing executable Contents/MacOS/Signal2M")
    module_path = bundle / "Contents/Resources/moduleinfo.json"
    require(module_path.is_file() and module_path.stat().st_size <= 256 * 1024, "Missing or oversized moduleinfo.json")
    module = tolerant_module_json(module_path.read_text(encoding="utf-8"))
    require(module.get("Name") == "Signal2M" and module.get("Version") == VERSION, "Unexpected module name/version")
    require(module.get("Factory Info", {}).get("Vendor") == "LMZ", "Unexpected VST3 factory vendor")
    classes = module.get("Classes")
    require(isinstance(classes, list) and len(classes) == len(CLASS_IDS), "Unexpected VST3 factory class list")
    categories = set()
    for item in classes:
        require(isinstance(item, dict), "Invalid VST3 class metadata")
        category = item.get("Category")
        require(category in CLASS_IDS and category not in categories, "Unexpected or duplicate VST3 class category")
        categories.add(category)
        require(str(item.get("CID", "")).upper() == CLASS_IDS[category], "VST3 class identity changed: " + category)
        require(item.get("Name") == "LMZ Signal 2M" and item.get("Vendor") == "LMZ" and item.get("Version") == VERSION,
                "Unexpected host-visible class name/vendor/version: " + category)
        if category == "Audio Module Class":
            require(set(item.get("Sub Categories", [])) == {"Instrument", "Synth"}, "Unexpected instrument VST3 subcategories")
    return executable


def version_tuple(value: str) -> tuple[int, ...]:
    numbers = tuple(int(part) for part in value.split("."))
    return (numbers + (0, 0, 0))[:3]


def macos_minimum_versions(otool_output: str) -> list[str]:
    """Read OS minimum fields, excluding nested build-tool version fields."""
    fields = {"LC_BUILD_VERSION": "minos", "LC_VERSION_MIN_MACOSX": "version"}
    command = ""
    versions = []
    for line in otool_output.splitlines():
        if re.match(r"\s*Load command\s+\d+\s*$", line):
            command = ""
        match = re.match(r"\s*cmd\s+(LC_\w+)\s*$", line)
        if match:
            command = match.group(1)
        field = fields.get(command)
        if field is not None:
            match = re.match(rf"\s*{field}\s+([0-9.]+)\s*$", line)
            if match:
                versions.append(match.group(1))
    return versions


def verify_macho(executable: Path) -> dict:
    architectures = set(bootstrap.run(["/usr/bin/lipo", "-archs", str(executable)], capture=True).split())
    require(architectures == ARCHITECTURES, f"Expected arm64+x86_64 Mach-O; found {sorted(architectures)}")
    report = {}
    for architecture in sorted(ARCHITECTURES):
        commands = bootstrap.run(["/usr/bin/otool", "-arch", architecture, "-l", str(executable)], capture=True)
        versions = macos_minimum_versions(commands)
        require(len(versions) == 1 and version_tuple(versions[0]) == MINIMUM_VERSIONS[architecture],
                f"Unexpected {architecture} minimum macOS version: {versions}")
        libraries = bootstrap.run(["/usr/bin/otool", "-arch", architecture, "-L", str(executable)], capture=True)
        dependencies = []
        for line in libraries.splitlines():
            match = re.match(r"\s+(\S+)\s+\(compatibility version ", line)
            if match:
                dependency = match.group(1)
                require(dependency.startswith(("/usr/lib/", "/System/Library/Frameworks/")),
                        f"Non-system dependency in {architecture}: {dependency}")
                dependencies.append(dependency)
        require(bool(dependencies), "otool did not report dynamic dependencies for " + architecture)
        report[architecture] = {"minimum_macos": versions[0], "dynamic_dependencies": dependencies}
    return report


def validate_bundle_tree(bundle: Path, executable: Path) -> None:
    root = bundle.resolve()
    for path in bundle.rglob("*"):
        mode = path.lstat().st_mode
        if stat.S_ISLNK(mode):
            require(path.resolve().is_relative_to(root), "Bundle symlink leaves the bundle: " + str(path))
        elif stat.S_ISREG(mode):
            with path.open("rb") as source:
                magic = source.read(4)
            require(magic not in MACHO_MAGICS or path == executable,
                    "Unexpected embedded Mach-O; define an explicit inside-out signing recipe before packaging: " + str(path))
        else:
            require(stat.S_ISDIR(mode), "Special file is not allowed in a plugin bundle: " + str(path))


def sign_bundle(bundle: Path, identity: str) -> dict:
    command = ["/usr/bin/codesign", "--force", "--sign", identity]
    if identity == "-":
        command.append("--timestamp=none")
    else:
        command.extend(["--options", "runtime", "--timestamp"])
    bootstrap.run([*command, str(bundle)])
    bootstrap.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", "--all-architectures", "--verbose=2", str(bundle)])
    description = bootstrap.run(["/usr/bin/codesign", "--display", "--verbose=4", str(bundle)], capture=True)
    if identity == "-":
        require("Signature=adhoc" in description, "Expected an ad-hoc code signature")
        return {"kind": "ad-hoc", "developer_id": False, "notarization": "not-performed"}
    require("Authority=Developer ID Application:" in description, "The selected signing identity is not Developer ID Application")
    require("TeamIdentifier=not set" not in description and "TeamIdentifier=" in description, "Missing Developer ID team identifier")
    return {"kind": "developer-id", "developer_id": True, "notarization": "not-performed"}


def license_files() -> dict[str, str]:
    result = {
        "dependencies/JUCE/LICENSE.md": "licenses/JUCE-LICENSE.md",
        "dependencies/JUCE/README.md": "licenses/JUCE-README.md",
        "dependencies/JUCE/modules/juce_audio_processors/format_types/VST3_SDK/LICENSE.txt": "licenses/VST3-SDK-LICENSE.txt",
        "dependencies/JUCE/modules/juce_audio_processors/format_types/VST3_SDK/pluginterfaces/LICENSE.txt": "licenses/VST3-PLUGINTERFACES-LICENSE.txt",
        "dependencies/JUCE/modules/juce_audio_processors/format_types/VST3_SDK/base/LICENSE.txt": "licenses/VST3-BASE-LICENSE.txt",
        "dependencies/JUCE/modules/juce_audio_processors/format_types/VST3_SDK/public.sdk/LICENSE.txt": "licenses/VST3-PUBLIC-SDK-LICENSE.txt",
        "dependencies/JUCE/modules/juce_audio_processors/format_types/VST3_SDK/VST3_License_Agreement.pdf": "licenses/VST3-License-Agreement.pdf",
        "dependencies/JUCE/modules/juce_core/zip/zlib/zlib.h": "licenses/ZLIB-NOTICE.h",
        "dependencies/JUCE/modules/juce_graphics/image_formats/pnglib/LICENSE": "licenses/PNG-LICENSE.txt",
        "dependencies/JUCE/modules/juce_graphics/image_formats/jpglib/README": "licenses/JPEG-README.txt",
        "dependencies/JUCE/modules/juce_audio_formats/codecs/flac/Flac Licence.txt": "licenses/FLAC-LICENSE.txt",
        "dependencies/JUCE/modules/juce_audio_formats/codecs/oggvorbis/Ogg Vorbis Licence.txt": "licenses/OGG-VORBIS-LICENSE.txt",
        "dependencies/JUCE/modules/juce_audio_formats/codecs/oggvorbis/libvorbis-1.3.7/COPYING": "licenses/LIBVORBIS-COPYING.txt",
    }
    for module in ("juce_core", "juce_audio_basics", "juce_audio_devices", "juce_events", "juce_graphics", "juce_gui_basics",
                   "juce_gui_extra", "juce_audio_formats", "juce_audio_processors", "juce_audio_utils", "juce_audio_plugin_client", "juce_data_structures"):
        result[f"dependencies/JUCE/modules/{module}/{module}.h"] = f"licenses/JUCE-modules/{module}.h"
    return result


def copy_documents(delivery: Path) -> None:
    documents = {
        "docs/INSTALL_MACOS_RU.txt": "INSTALL_MACOS_RU.txt",
        "THIRD_PARTY_NOTICES_MACOS.md": "THIRD_PARTY_NOTICES_MACOS.md",
    }
    if (ROOT / "REVIEW_MACOS_REPORT.md").is_file():
        documents["REVIEW_MACOS_REPORT.md"] = "REVIEW_MACOS_REPORT.md"
    for source_name, destination_name in {**documents, **license_files()}.items():
        source = ROOT / source_name
        require(source.is_file(), "Missing macOS delivery document/license: " + source_name)
        if source.suffix.lower() != ".pdf":
            source.read_text(encoding="utf-8")
        destination = delivery / destination_name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    for name in ("lmz-logo.svg", "toggle-on.svg", "toggle-off.svg", "LOGO_SOURCE.md"):
        source = ROOT / "assets" / name
        source.read_text(encoding="utf-8")
        destination = delivery / "artwork" / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)


def payload_inventory(delivery: Path) -> tuple[dict, dict, dict]:
    files, symlinks, modes = {}, {}, {}
    for path in sorted(delivery.rglob("*")):
        relative = path.relative_to(delivery).as_posix()
        mode = path.lstat().st_mode
        if stat.S_ISLNK(mode):
            symlinks[relative] = os.readlink(path)
        elif stat.S_ISREG(mode):
            files[relative] = sha256(path)
        elif stat.S_ISDIR(mode):
            continue
        else:
            raise bootstrap.BootstrapError("Unexpected special delivery file: " + relative)
        modes[relative] = stat.S_IMODE(mode)
    return files, symlinks, modes


def verify_zip(archive_path: Path, manifest: dict) -> None:
    prefix = DELIVERY_NAME + "/"
    with zipfile.ZipFile(archive_path) as archive:
        members = archive.infolist()
        require(len({item.filename for item in members}) == len(members), "Duplicate ZIP members")
        require(archive.testzip() is None, "ZIP CRC check failed")
        actual = set()
        for item in members:
            relative = PurePosixPath(item.filename)
            require(not relative.is_absolute() and ".." not in relative.parts and "\\" not in item.filename,
                    "Unsafe ZIP entry: " + item.filename)
            require(item.filename.startswith((prefix, "__MACOSX/")), "Unexpected ZIP root: " + item.filename)
            if item.is_dir() or item.filename.startswith("__MACOSX/"):
                continue
            name = item.filename[len(prefix):]
            actual.add(name)
            if name == "MANIFEST.json":
                require(json.loads(archive.read(item).decode("utf-8")) == manifest, "Archived manifest mismatch")
                continue
            mode = (item.external_attr >> 16) & 0xFFFF
            require(stat.S_IMODE(mode) == manifest["modes"].get(name), "ZIP changed POSIX permissions: " + name)
            if name in manifest["symlinks"]:
                require(stat.S_ISLNK(mode) and archive.read(item).decode("utf-8") == manifest["symlinks"][name], "ZIP symlink mismatch: " + name)
            else:
                require(stat.S_ISREG(mode) and name in manifest["files"], "Unexpected ZIP payload: " + name)
                require(hashlib.sha256(archive.read(item)).hexdigest() == manifest["files"][name], "ZIP hash mismatch: " + name)
        require(actual == set(manifest["files"]) | set(manifest["symlinks"]) | {"MANIFEST.json"}, "ZIP payload inventory mismatch")


def load_test_results(path: Path | None, executable: Path) -> dict:
    if path is None:
        return {"ctest": "not-recorded", "manual_daw_and_visual_checks": "pending"}
    result = json.loads(path.read_text(encoding="utf-8"))
    require(result.get("ctest") == "passed" and result.get("version") == VERSION, "Native test report did not pass this release")
    require(result.get("native_test_architecture") in ARCHITECTURES, "Invalid native test architecture")
    require(result.get("source_tested_executable_sha256") == sha256(executable), "Native test report refers to a different executable")
    return result


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, default=ROOT / "build-macos/Signal2M_artefacts/Release/VST3/Signal2M.vst3")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "dist-macos")
    parser.add_argument("--sign-identity", default="-", help="Explicit installed Developer ID Application identity; default '-' is local ad-hoc")
    parser.add_argument("--test-results", type=Path, help="Native report emitted by tools/build_macos.py for the same executable")
    parser.add_argument("--vst3-checker", type=Path, default=ROOT / "build-macos/Signal2MMacVst3Checks", help="Native independent VST3 checker for the actual archived bundle")
    arguments = parser.parse_args()
    try:
        require(sys.platform == "darwin", "macOS packaging requires the native lipo, otool, codesign and ditto tools")
        require(bool(arguments.sign_identity.strip()), "Signing identity cannot be empty; use '-' for ad-hoc testing")
        native_arch = bootstrap.check_native_environment()
        checker = arguments.vst3_checker.resolve()
        require(checker.is_file(), "Build the independent Signal2MMacVst3Checks target before packaging")
        source_bundle = arguments.bundle.resolve()
        source_executable = verify_metadata(source_bundle)
        validate_bundle_tree(source_bundle, source_executable)
        macho = verify_macho(source_executable)
        test_results = load_test_results(arguments.test_results, source_executable)
        output_directory = arguments.output_dir.resolve()
        require(output_directory != (ROOT / "dist").resolve(), "Use dist-macos for macOS; the Windows delivery in dist is preserved")
        output_directory.mkdir(parents=True, exist_ok=True)
        output = output_directory / (DELIVERY_NAME + ".zip")
        with tempfile.TemporaryDirectory(prefix=".signal2m-mac-", dir=output_directory) as temporary:
            staging = Path(temporary)
            delivery = staging / DELIVERY_NAME
            delivery.mkdir()
            bundle = delivery / "Signal2M.vst3"
            bootstrap.run(["/usr/bin/ditto", "--rsrc", "--extattr", str(source_bundle), str(bundle)])
            signature = sign_bundle(bundle, arguments.sign_identity)
            verify_metadata(bundle)
            copy_documents(delivery)
            files, symlinks, modes = payload_inventory(delivery)
            manifest = {
                "product": "LMZ Signal 2M", "version": VERSION, "platform": "macOS Universal",
                "format": "VST3", "architectures": sorted(ARCHITECTURES), "bundle_id": "sound.lmz.signal2m",
                "macho": macho, "signing": signature, "native_checks": test_results,
                "delivered_signed_executable_sha256": sha256(bundle / "Contents/MacOS/Signal2M"),
                # No ZIP is published unless the extracted host probe below
                # succeeds for this exact signed executable and architecture.
                "delivery_vst3_check": {"architecture": native_arch, "status": "passed"},
                "manual_daw_and_visual_checks": "pending", "files": files, "symlinks": symlinks, "modes": modes,
            }
            (delivery / "MANIFEST.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            temporary_zip = staging / output.name
            bootstrap.run(["/usr/bin/ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", str(delivery), str(temporary_zip)])
            verify_zip(temporary_zip, manifest)
            # Verify the signed bundle reconstructed from the actual archive as
            # well as the stage. This catches lost mode bits, links and seals.
            extracted = staging / "archive-check"
            bootstrap.run(["/usr/bin/ditto", "-x", "-k", str(temporary_zip), str(extracted)])
            extracted_bundle = extracted / DELIVERY_NAME / "Signal2M.vst3"
            verify_metadata(extracted_bundle)
            bootstrap.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", "--all-architectures", "--verbose=2", str(extracted_bundle)])
            bootstrap.run([
                "/usr/bin/arch", "-" + native_arch, str(checker), str(extracted_bundle),
                str(ROOT / "Tests/Fixtures/Signal2M-v0.1-state.bin"),
            ])
            reviewer = ROOT / "Tests/ReviewerMacPackageChecks.py"
            if reviewer.is_file():
                temporary_zip.with_suffix(".zip.sha256").write_text(f"{sha256(temporary_zip)}  {temporary_zip.name}\n", encoding="utf-8")
                bootstrap.run([sys.executable, "-X", "utf8", str(reviewer), "--bundle", str(bundle), "--zip", str(temporary_zip)])
            temporary_zip.replace(output)
        checksum = sha256(output)
        output.with_suffix(".zip.sha256").write_text(f"{checksum}  {output.name}\n", encoding="utf-8")
        print(f"Packaged {output} ({output.stat().st_size} bytes)")
        print("Archive SHA-256: " + checksum)
        print("Signing: " + signature["kind"] + "; notarization: not performed")
    except (bootstrap.BootstrapError, OSError, ValueError, zipfile.BadZipFile) as error:
        print("macOS packaging failed: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
