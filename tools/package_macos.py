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
import macos_release as release


ROOT = bootstrap.ROOT
VERSION = release.project_version(ROOT)
DELIVERY_NAME = f"Signal2M-v{VERSION}-macOS-Universal"
BUNDLE_NAME = "Signal2M.vst3"
EXECUTABLE_PATH = "Contents/MacOS/Signal2M"
BUILD_PATH = "Contents/Resources/Build"
MANIFEST_PATH = BUILD_PATH + "/MANIFEST.json"
LEGAL_PATH = "Contents/Resources/Legal"
INVENTORY_EXCLUSIONS = [EXECUTABLE_PATH, "Contents/_CodeSignature/**", MANIFEST_PATH]
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




def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def prepare_resources(bundle: Path, test_results: dict, provenance: dict) -> None:
    legal = bundle / LEGAL_PATH
    build = bundle / BUILD_PATH
    require(not legal.exists() and not build.exists(), "Source bundle already contains packaged release resources")
    documents = {"THIRD_PARTY_NOTICES_MACOS.md": "THIRD_PARTY_NOTICES_MACOS.md",
                 "assets/LOGO_SOURCE.md": "LOGO_SOURCE.md"}
    for source_name, destination_name in {**documents, **license_files()}.items():
        source = ROOT / source_name
        require(source.is_file(), "Missing macOS dependency notice: " + source_name)
        if source.suffix.lower() != ".pdf":
            source.read_text(encoding="utf-8")
        destination = legal / destination_name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    write_json(build / "native-test-results.json", test_results)
    write_json(build / "provenance.json", provenance)


def excluded_from_manifest(name: str) -> bool:
    return name in (EXECUTABLE_PATH, MANIFEST_PATH) or name.startswith("Contents/_CodeSignature/")


def payload_inventory(bundle: Path, *, manifest_scope: bool = False) -> tuple[dict, dict, dict]:
    files, symlinks, modes = {}, {}, {}
    bundle_root = bundle.resolve()
    for path in sorted(bundle.rglob("*")):
        relative = path.relative_to(bundle).as_posix()
        if manifest_scope and excluded_from_manifest(relative):
            continue
        mode = path.lstat().st_mode
        if stat.S_ISLNK(mode):
            target = os.readlink(path)
            require(not PurePosixPath(target).is_absolute() and ":" not in target and "\\" not in target,
                    "Bundle symlink must use a relative POSIX target: " + relative)
            require(path.resolve().is_relative_to(bundle_root), "Bundle symlink leaves the bundle: " + relative)
            symlinks[relative] = target
        elif stat.S_ISREG(mode):
            files[relative] = sha256(path)
        elif stat.S_ISDIR(mode):
            continue
        else:
            raise bootstrap.BootstrapError("Unexpected special bundle file: " + relative)
        modes[relative] = stat.S_IMODE(mode)
    return files, symlinks, modes


def verify_internal_manifest(bundle: Path, manifest: dict) -> None:
    require(manifest["inventory_exclusions"] == INVENTORY_EXCLUSIONS, "Unexpected manifest hash exclusions")
    files, symlinks, modes = payload_inventory(bundle, manifest_scope=True)
    require((manifest["files"], manifest["symlinks"], manifest["modes"]) == (files, symlinks, modes),
            "Signed bundle resources differ from the pre-sign manifest")
    require(json.loads((bundle / MANIFEST_PATH).read_text(encoding="utf-8")) == manifest,
            "Embedded manifest was changed")


def verify_zip(archive_path: Path, inventory: tuple[dict, dict, dict]) -> None:
    files, symlinks, modes = inventory
    prefix = BUNDLE_NAME + "/"
    with zipfile.ZipFile(archive_path) as archive:
        members = archive.infolist()
        names = [item.filename for item in members]
        require(len(set(names)) == len(names) and len({name.casefold() for name in names}) == len(names),
                "Duplicate or case-colliding ZIP members")
        require(archive.testzip() is None, "ZIP CRC check failed")
        actual = set()
        for item in members:
            name = item.filename.rstrip("/")
            relative = PurePosixPath(name)
            require(name == relative.as_posix() and not relative.is_absolute() and ".." not in relative.parts
                    and "\\" not in name and ":" not in name and "\0" not in name, "Unsafe ZIP entry: " + item.filename)
            require(item.filename.startswith(prefix), "ZIP must contain only the Signal2M.vst3 bundle: " + item.filename)
            if item.is_dir():
                continue
            name = item.filename[len(prefix):]
            actual.add(name)
            mode = (item.external_attr >> 16) & 0xFFFF
            require(stat.S_IMODE(mode) == modes.get(name), "ZIP changed POSIX permissions: " + name)
            if name in symlinks:
                require(stat.S_ISLNK(mode) and archive.read(item).decode("utf-8") == symlinks[name], "ZIP symlink mismatch: " + name)
            else:
                require(stat.S_ISREG(mode) and name in files, "Unexpected ZIP payload: " + name)
                require(hashlib.sha256(archive.read(item)).hexdigest() == files[name], "ZIP hash mismatch: " + name)
        require(actual == set(files) | set(symlinks), "ZIP payload inventory mismatch")


def load_test_results(path: Path | None, executable: Path, native_arch: str) -> dict:
    require(path is not None, "A release requires the native test report from tools/build_macos.py")
    result = json.loads(path.read_text(encoding="utf-8"))
    require(result.get("ctest") == "passed" and result.get("version") == VERSION, "Native tests did not pass this release")
    require(result.get("platform") == "macOS" and result.get("universal_binary_architectures") == sorted(ARCHITECTURES),
            "Native test report must describe the macOS Universal build")
    require(result.get("native_test_architecture") == native_arch, "Native test report uses a different runtime architecture")
    require(release.EXPECTED_TESTS.issubset(set(result.get("tests", []))), "Native test report is missing mandatory suites")
    require(result.get("release_channel") == "release" and result.get("show_beta") is False, "Release must disable the BETA label")
    require(result.get("mac_update_gid") == release.MAC_UPDATE_GID and result.get("donation_gid") == release.DONATION_GID,
            "Native test report has incorrect update/donation tabs")
    require(result.get("source_tested_executable_sha256") == sha256(executable), "Native test report refers to a different executable")
    return result


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, default=ROOT / "build-macos/Signal2M_artefacts/Release/VST3/Signal2M.vst3")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "dist-macos")
    parser.add_argument("--sign-identity", default="-", help="Explicit installed Developer ID Application identity; default '-' is ad-hoc")
    parser.add_argument("--test-results", type=Path, required=True, help="Native report emitted by tools/build_macos.py for this executable")
    parser.add_argument("--vst3-checker", type=Path, default=ROOT / "build-macos/Signal2MMacVst3Checks")
    parser.add_argument("--report", type=Path, default=ROOT / "build-macos/package-results.json", help="External CI evidence; excluded from the user ZIP")
    arguments = parser.parse_args()
    try:
        require(sys.platform == "darwin", "macOS packaging requires native lipo, otool, codesign and ditto")
        require(bool(arguments.sign_identity.strip()), "Use '-' for ad-hoc testing or specify an installed signing identity")
        native_arch = bootstrap.check_native_environment()
        checker = arguments.vst3_checker.resolve()
        reviewer = ROOT / "Tests/ReviewerMacPackageChecks.py"
        require(checker.is_file() and reviewer.is_file(), "Build and retain the independent Mac release checkers")
        source_bundle = arguments.bundle.resolve()
        source_executable = verify_metadata(source_bundle)
        validate_bundle_tree(source_bundle, source_executable)
        macho = verify_macho(source_executable)
        test_results = load_test_results(arguments.test_results, source_executable, native_arch)
        provenance = release.source_provenance(ROOT)
        output_directory = arguments.output_dir.resolve()
        require(output_directory != (ROOT / "dist").resolve(), "Keep Mac releases in dist-macos")
        output_directory.mkdir(parents=True, exist_ok=True)
        output = output_directory / (DELIVERY_NAME + ".zip")
        with tempfile.TemporaryDirectory(prefix=".signal2m-mac-", dir=output_directory) as temporary:
            staging = Path(temporary)
            bundle = staging / BUNDLE_NAME
            bootstrap.run(["/usr/bin/ditto", "--norsrc", "--noextattr", str(source_bundle), str(bundle)])
            prepare_resources(bundle, test_results, provenance)
            files, symlinks, modes = payload_inventory(bundle, manifest_scope=True)
            manifest = {
                "schema_version": 2, "product": "LMZ Signal 2M", "version": VERSION, "platform": "macOS Universal",
                "format": "VST3", "architectures": sorted(ARCHITECTURES), "bundle_id": "sound.lmz.signal2m",
                "release_channel": "release", "show_beta": False,
                "macho": macho, "native_checks": test_results, "provenance": provenance,
                "source_tested_executable_sha256": test_results["source_tested_executable_sha256"],
                "signing_policy": {"kind": "ad-hoc" if arguments.sign_identity == "-" else "developer-id",
                                   "notarization": "not-performed", "verification_required_before_publish": True},
                "delivery_validation_policy": {"native_architecture": native_arch,
                    "required": ["full-zip-byte-mode-link-match", "extracted-codesign", "extracted-native-vst3-host", "independent-package-review"]},
                "hash_scope": "bundle-relative pre-sign resource inventory; final executable, signature and manifest are excluded to avoid circular hashes",
                "inventory_exclusions": INVENTORY_EXCLUSIONS,
                "manual_daw_and_visual_checks": "pending", "files": files, "symlinks": symlinks, "modes": modes,
            }
            write_json(bundle / MANIFEST_PATH, manifest)
            # Every release resource is present before the final signature.
            signature = sign_bundle(bundle, arguments.sign_identity)
            verify_metadata(bundle)
            validate_bundle_tree(bundle, bundle / EXECUTABLE_PATH)
            verify_internal_manifest(bundle, manifest)
            signed_inventory = payload_inventory(bundle)
            temporary_zip = staging / output.name
            bootstrap.run(["/usr/bin/ditto", "-c", "-k", "--norsrc", "--noextattr", "--keepParent", str(bundle), str(temporary_zip)])
            verify_zip(temporary_zip, signed_inventory)
            extracted = staging / "archive-check"
            bootstrap.run(["/usr/bin/ditto", "-x", "-k", str(temporary_zip), str(extracted)])
            require({path.name for path in extracted.iterdir()} == {BUNDLE_NAME}, "Archive extraction created extra top-level entries")
            extracted_bundle = extracted / BUNDLE_NAME
            verify_metadata(extracted_bundle)
            require(payload_inventory(extracted_bundle) == signed_inventory, "Extracted bundle bytes/modes/links changed")
            verify_internal_manifest(extracted_bundle, manifest)
            bootstrap.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", "--all-architectures", "--verbose=2", str(extracted_bundle)])
            bootstrap.run(["/usr/bin/arch", "-" + native_arch, str(checker), str(extracted_bundle),
                           str(ROOT / "Tests/Fixtures/Signal2M-v0.1-state.bin")])
            require(payload_inventory(extracted_bundle) == signed_inventory, "Native host changed the extracted signed bundle")
            checksum = sha256(temporary_zip)
            temporary_zip.with_suffix(".zip.sha256").write_text(f"{checksum}  {temporary_zip.name}\n", encoding="utf-8")
            bootstrap.run([sys.executable, "-X", "utf8", str(reviewer), "--bundle", str(bundle), "--zip", str(temporary_zip)])
            report = {
                "product": "LMZ Signal 2M", "version": VERSION, "release_channel": "release", "show_beta": False,
                "native_checks": test_results, "provenance": provenance, "signing": signature,
                "delivery_vst3_check": {"architecture": native_arch, "status": "passed"},
                "archive": {"name": output.name, "sha256": checksum, "bytes": temporary_zip.stat().st_size, "root": BUNDLE_NAME},
                "delivered_signed_executable_sha256": sha256(bundle / EXECUTABLE_PATH),
                "files": signed_inventory[0], "symlinks": signed_inventory[1], "modes": signed_inventory[2],
            }
            temporary_zip.replace(output)
        output.with_suffix(".zip.sha256").write_text(f"{checksum}  {output.name}\n", encoding="utf-8")
        write_json(arguments.report, report)
        print(f"Packaged {output} ({output.stat().st_size} bytes)")
        print("Archive SHA-256: " + checksum)
        print("Signing: " + signature["kind"] + "; notarization: not performed")
    except (bootstrap.BootstrapError, OSError, ValueError, zipfile.BadZipFile) as error:
        print("macOS packaging failed: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

