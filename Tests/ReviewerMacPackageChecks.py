"""Independent macOS delivery checks; all text I/O is explicit UTF-8.

--self-test validates this parser against generated in-memory Mach-O cases. It
does not build, load, sign, or approve a macOS plug-in. A real delivery requires
--bundle and, for archive approval, --zip on macOS. --offline is structural only.
Mach-O structures: Apple's xnu EXTERNAL_HEADERS/mach-o/{fat,loader}.h.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import re
import stat
import struct
import subprocess
import sys
import zipfile

VERSION = "0.4.0"
HOST_NAME = "LMZ Signal 2M"
TOP = "Signal2M.vst3"
CPUS = {0x01000007: ("x86_64", (10, 15, 0)), 0x0100000C: ("arm64", (11, 0, 0))}
CLASSES = {
    "Audio Module Class": "abcdef019182faeb4c6d7a7a5367326d",
    "Component Controller Class": "abcdef011234abcd4c6d7a7a5367326d",
    "Plugin Compatibility Class": "abcdef01c0def00d4c6d7a7a5367326d",
}
MAC_FEED = ("https://docs.google.com/spreadsheets/d/"
            "1ZyDHZskIvieqR_L6y4Ghu0gwvc6usByiNUF52y6AidM/"
            "export?format=csv&gid=640439888&range=A1%3AB1").encode("ascii")
WIN_FEED = MAC_FEED.replace(b"gid=640439888", b"gid=0")
DONATION_FEED = MAC_FEED.replace(b"gid=640439888&range=A1%3AB1", b"gid=1525027926&range=A1%3AA2")
MANIFEST_PATH = "Contents/Resources/Build/MANIFEST.json"
NATIVE_PATH = "Contents/Resources/Build/native-test-results.json"
PROVENANCE_PATH = "Contents/Resources/Build/provenance.json"
EXCLUSIONS = ["Contents/MacOS/Signal2M", "Contents/_CodeSignature/**", MANIFEST_PATH]
EXPECTED_TESTS = {"Signal2MDspBehavior", "Signal2MMidiNoteVoice", "Signal2MIndependentDsp",
                  "Signal2MParametersAndState", "Signal2MManualUpdateChecker", "Signal2MDonationLinks",
                  "Signal2MRealMacVst3Module", "Signal2MMacUpdatePolicy", "Signal2MMacPackageParser",
                  "Signal2MRealMacUpdateLifecycle"}



def require(condition: bool, description: str) -> None:
    if not condition:
        raise ValueError(description)


def passed(description: str) -> None:
    print("PASS " + description)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def version(value: int) -> tuple[int, int, int]:
    return value >> 16, (value >> 8) & 255, value & 255


def unpack(fmt: str, data: bytes, offset: int) -> tuple:
    require(0 <= offset <= len(data) - struct.calcsize(fmt), "truncated Mach-O structure")
    return struct.unpack_from(fmt, data, offset)


def command_string(command: bytes, endian: str, minimum: int) -> str:
    offset, = unpack(endian + "I", command, 8)
    require(minimum <= offset < len(command), "invalid load-command string offset")
    end = command.find(b"\0", offset)
    require(end >= offset, "unterminated load-command string")
    return command[offset:end].decode("utf-8", errors="strict")


def thin_macho(data: bytes, outer_cpu: int, outer_subtype: int) -> dict:
    require(data[:4] == b"\xcf\xfa\xed\xfe", "each supported slice must be little-endian Mach-O 64")
    _, cpu, subtype, filetype, count, size, _, reserved = unpack("<8I", data, 0)
    require(cpu == outer_cpu and subtype == outer_subtype, "fat table and slice CPU/subtype disagree")
    require(cpu in CPUS and filetype == 8 and reserved == 0, "slice is not a supported macOS MH_BUNDLE")
    require(0 < count <= size // 8 and size <= len(data) - 32, "invalid Mach-O load-command bounds")
    end = 32 + size
    offset = 32
    minimum = None
    dependencies = []
    signature = False
    for _ in range(count):
        command, length = unpack("<II", data, offset)
        require(length >= 8 and length % 8 == 0 and offset + length <= end, "invalid load-command size")
        record = data[offset:offset + length]
        if command in (0xC, 0x80000018, 0x8000001F, 0x20, 0x80000023):
            require(length >= 24, "truncated dylib command")
            name = command_string(record, "<", 24)
            require(name.startswith(("/System/Library/", "/usr/lib/")),
                    "non-system runtime dependency: " + name)
            require(not re.search(r"(?i)(?:winhttp|ucrt|vcruntime|msvcp|mingw|\.dll(?:$|/))", name),
                    "Windows runtime dependency in macOS slice")
            dependencies.append(name)
        elif command == 0x24:
            require(length == 16 and minimum is None, "invalid/duplicate deployment target")
            minimum, sdk = unpack("<II", record, 8)
            require(version(sdk) >= version(minimum), "SDK precedes deployment target")
        elif command == 0x32:
            require(length >= 24 and minimum is None, "invalid/duplicate deployment target")
            platform, minimum, sdk, tools = unpack("<4I", record, 8)
            require(platform == 1 and length == 24 + tools * 8, "slice is not a macOS build-version command")
            require(version(sdk) >= version(minimum), "SDK precedes deployment target")
        elif command == 0x1D:
            require(length == 16 and not signature, "invalid/duplicate signature command")
            start, length_bytes = unpack("<II", record, 8)
            require(start >= end and length_bytes >= 12 and start + length_bytes <= len(data),
                    "code-signature blob is outside the slice")
            require(data[start:start + 4] == b"\xfa\xde\x0c\xc0", "missing embedded code-signature superblob")
            signature = True
        elif command == 0x27:
            raise ValueError("embedded DYLD environment is not portable")
        offset += length
    require(offset == end, "load-command count does not consume declared command bytes")
    require(minimum is not None and version(minimum) == CPUS[cpu][1],
            "deployment target must be 10.15 Intel / 11.0 arm64")
    require(dependencies, "no Apple runtime dependency was found")
    require(signature, "slice lacks a code-signature command (cryptographic validation still required)")
    return {"architecture": CPUS[cpu][0], "minimum_os": version(minimum),
            "dependencies": dependencies, "sha256": sha(data)}


def universal_macho(data: bytes) -> list[dict]:
    magic = data[:4]
    formats = {b"\xca\xfe\xba\xbe": (">", False), b"\xbe\xba\xfe\xca": ("<", False),
               b"\xca\xfe\xba\xbf": (">", True), b"\xbf\xba\xfe\xca": ("<", True)}
    require(magic in formats, "delivery executable is not a universal Mach-O")
    endian, wide = formats[magic]
    count, = unpack(endian + "I", data, 4)
    require(count == 2, "universal bundle must contain exactly arm64 and x86_64 slices")
    entry_size = 32 if wide else 20
    table_end = 8 + count * entry_size
    require(table_end <= len(data), "truncated universal architecture table")
    found = set()
    ranges = []
    results = []
    for index in range(count):
        entry = unpack(endian + ("IIQQII" if wide else "5I"), data, 8 + index * entry_size)
        cpu, subtype, offset, size, alignment = entry[:5]
        require(cpu in CPUS and cpu not in found, "missing, duplicate, or unsupported architecture")
        require(not wide or entry[5] == 0, "nonzero fat_arch_64 reserved field")
        require(alignment <= 30 and offset % (1 << alignment) == 0, "invalid architecture alignment")
        require(offset >= table_end and size >= 32 and offset + size <= len(data), "architecture slice is outside the executable")
        require(all(offset + size <= a or offset >= b for a, b in ranges), "architecture slices overlap")
        found.add(cpu)
        ranges.append((offset, offset + size))
        result = thin_macho(data[offset:offset + size], cpu, subtype)
        result["bytes"] = data[offset:offset + size]
        results.append(result)
    require(found == set(CPUS), "universal architectures are not arm64 plus x86_64")
    return results


def sdk_json(text: str) -> dict:
    # The pinned Steinberg moduleinfotool emits trailing commas. Remove only
    # those outside strings; a URL containing ',]' must remain untouched.
    output = []
    quoted = escaped = False
    for index, char in enumerate(text):
        if quoted:
            output.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
        elif char == '"':
            quoted = True
            output.append(char)
        elif char == "," and text[index + 1:].lstrip().startswith(("}", "]")):
            continue
        else:
            output.append(char)
    return json.loads("".join(output))


def bundle_checks(bundle: Path, offline: bool) -> dict[str, bytes]:
    require(bundle.name == "Signal2M.vst3" and bundle.is_dir(), "supply the real Signal2M.vst3 bundle")
    info = plistlib.loads((bundle / "Contents/Info.plist").read_bytes())
    expected = {"CFBundleIdentifier": "sound.lmz.signal2m", "CFBundleName": "Signal2M",
                "CFBundleExecutable": "Signal2M", "CFBundlePackageType": "BNDL",
                "CFBundleShortVersionString": VERSION, "CFBundleVersion": VERSION}
    require(all(info.get(key) == value for key, value in expected.items()), "bundle metadata/identity/version mismatch")
    passed("Info.plist identity, physical names and 0.4.0 versions")
    executable = bundle / "Contents/MacOS/Signal2M"
    require(executable.is_file() and not executable.is_symlink(), "native executable must be a regular bundle file")
    slices = universal_macho(executable.read_bytes())
    for item in slices:
        require(MAC_FEED in item["bytes"] and WIN_FEED not in item["bytes"] and DONATION_FEED in item["bytes"],
                "incorrect Mac update/donation feed in " + item["architecture"])
        for text in ("Доступна новая версия", "Используемая версия: ", "lfo_tempo_sync", "delay_tempo_sync", "fx_post"):
            require(text.encode("utf-8") in item["bytes"], "missing current UI/parameter string: " + text)
        require(b"BETA\0" not in item["bytes"], "macOS release contains the compiled BETA badge literal")
        print("SLICE " + json.dumps({k: v for k, v in item.items() if k != "bytes"}, ensure_ascii=False))
    passed("two genuine Mach-O slices, deployment targets, Apple runtime dependencies and Mac A1:B1 endpoint")
    metadata = sdk_json((bundle / "Contents/Resources/moduleinfo.json").read_text(encoding="utf-8"))
    require(metadata.get("Version") == VERSION and metadata.get("Factory Info", {}).get("Vendor") == "LMZ", "moduleinfo factory/version mismatch")
    classes = metadata.get("Classes", [])
    require(len(classes) == len(CLASSES), "moduleinfo class count changed")
    require({item.get("Category") for item in classes} == set(CLASSES), "moduleinfo class categories changed")
    for item in classes:
        require(item.get("CID", "").lower() == CLASSES[item["Category"]]
                and item.get("Name") == HOST_NAME and item.get("Vendor") == "LMZ"
                and item.get("Version") == VERSION, "moduleinfo semantic class identity/name/version changed")
    passed("all factory class CIDs, host names and versions")
    regular = {}
    for path in bundle.rglob("*"):
        if path.is_file() and not path.is_symlink():
            relative = path.relative_to(bundle).as_posix()
            require(not relative.lower().endswith((".dll", ".exe", ".pdb", ".lib", ".obj")), "Windows/developer file in macOS bundle")
            regular[relative] = path.read_bytes()
    bundle_links = {p.relative_to(bundle).as_posix(): os.readlink(p) for p in bundle.rglob("*") if p.is_symlink()}
    bundle_modes = {p.relative_to(bundle).as_posix(): stat.S_IMODE(p.lstat().st_mode)
                    for p in bundle.rglob("*") if p.is_file() or p.is_symlink()}
    # Extracted offline files on Windows cannot preserve POSIX modes. The ZIP
    # path validates the manifest against recorded ZIP modes instead.
    if sys.platform == "darwin":
        manifest_checks(regular, bundle_links, bundle_modes)
    if not offline:
        require(sys.platform == "darwin", "native codesign verification requires macOS; --offline is structural only")
        result = subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", "--all-architectures", "--verbose=2", str(bundle)],
                                capture_output=True, encoding="utf-8", errors="strict", timeout=30)
        require(result.returncode == 0, "native codesign validation failed: " + result.stdout + result.stderr)
        passed("native codesign --verify --strict on the actual bundle")
    print("BINARY_SHA256 " + sha(executable.read_bytes()))
    return regular


def safe_relative(name: str) -> bool:
    parts = PurePosixPath(name).parts
    return bool(parts) and name == PurePosixPath(name).as_posix() and not PurePosixPath(name).is_absolute() and not any(p in ("", ".", "..") for p in parts) and ":" not in name and "\\" not in name and "\0" not in name


def inventory_excluded(name: str) -> bool:
    return name in (EXCLUSIONS[0], MANIFEST_PATH) or name.startswith("Contents/_CodeSignature/")


def manifest_checks(payload: dict[str, bytes], links: dict[str, str], modes: dict[str, int]) -> None:
    for name in (MANIFEST_PATH, NATIVE_PATH, PROVENANCE_PATH,
                 "Contents/Resources/Legal/THIRD_PARTY_NOTICES_MACOS.md"):
        require(name in payload, "missing embedded resource: " + name)
        require("\ufffd" not in payload[name].decode("utf-8", errors="strict"), "invalid delivery text: " + name)
    require(any(name.startswith("Contents/Resources/Legal/licenses/") for name in payload), "missing embedded original licenses")
    manifest = json.loads(payload[MANIFEST_PATH].decode("utf-8"))
    require(manifest.get("schema_version") == 2 and manifest.get("release_channel") == "release"
            and manifest.get("show_beta") is False
            and manifest.get("version") == VERSION and manifest.get("product") == HOST_NAME
            and manifest.get("format") == "VST3" and manifest.get("platform") == "macOS Universal"
            and manifest.get("architectures") == ["arm64", "x86_64"]
            and manifest.get("bundle_id") == "sound.lmz.signal2m", "manifest identity mismatch")
    require(manifest.get("inventory_exclusions") == EXCLUSIONS, "manifest circular-signature inventory scope changed")
    require(manifest.get("hash_scope") == "bundle-relative pre-sign resource inventory; final executable, signature and manifest are excluded to avoid circular hashes",
            "manifest must describe its pre-sign resource scope")
    policy = manifest.get("signing_policy", {})
    require(policy.get("kind") in ("ad-hoc", "developer-id") and policy.get("notarization") == "not-performed"
            and policy.get("verification_required_before_publish") is True, "invalid signing policy")
    delivery = manifest.get("delivery_validation_policy", {})
    require(delivery.get("native_architecture") in ("arm64", "x86_64")
            and delivery.get("required") == ["full-zip-byte-mode-link-match", "extracted-codesign", "extracted-native-vst3-host", "independent-package-review"],
            "missing post-archive verification requirements")
    require(manifest.get("files") == {name: sha(data) for name, data in payload.items() if not inventory_excluded(name)},
            "manifest does not account for every scoped resource byte")
    require(manifest.get("symlinks", {}) == {name: target for name, target in links.items() if not inventory_excluded(name)},
            "manifest symlink inventory mismatch")
    require(manifest.get("modes") == {name: mode for name, mode in modes.items() if not inventory_excluded(name)},
            "manifest permission inventory mismatch")
    native = json.loads(payload[NATIVE_PATH].decode("utf-8"))
    require(native.get("version") == VERSION and native.get("platform") == "macOS"
            and native.get("release_channel") == "release" and native.get("show_beta") is False
            and native.get("ctest") == "passed" and EXPECTED_TESTS.issubset(native.get("tests", []))
            and native.get("native_test_architecture") in ("arm64", "x86_64")
            and native.get("universal_binary_architectures") == ["arm64", "x86_64"]
            and native.get("mac_update_gid") == "640439888" and native.get("donation_gid") == "1525027926",
            "embedded report does not attest all ten native release tests and correct feeds")
    require(delivery.get("native_architecture") == native.get("native_test_architecture"), "delivery/native architecture provenance mismatch")
    source_hash = native.get("source_tested_executable_sha256", "")
    require(re.fullmatch(r"[0-9a-f]{64}", source_hash) is not None, "invalid tested-source executable SHA-256")
    require(manifest.get("native_checks") == native
            and manifest.get("source_tested_executable_sha256") == source_hash, "manifest/native report provenance mismatch")
    provenance = json.loads(payload[PROVENANCE_PATH].decode("utf-8"))
    require(manifest.get("provenance") == provenance, "manifest/source provenance mismatch")
    require(provenance.get("version") == VERSION and provenance.get("release_channel") == "release"
            and provenance.get("show_beta") is False and isinstance(provenance.get("source_files"), dict)
            and bool(provenance["source_files"]), "invalid embedded source provenance")
    require(all(safe_relative(name) and re.fullmatch(r"[0-9a-f]{64}", digest)
                for name, digest in provenance["source_files"].items()), "invalid source hash inventory")


def zip_checks(path: Path, bundle_files: dict[str, bytes], bundle: Path, *, quiet: bool = False) -> None:
    checksum = path.with_suffix(path.suffix + ".sha256").read_text(encoding="utf-8").split()[0]
    require(checksum == sha(path.read_bytes()), "archive SHA-256 sidecar mismatch")
    with zipfile.ZipFile(path) as archive:
        require(archive.testzip() is None, "archive CRC failure")
        names = archive.namelist()
        require(len(names) == len(set(names)) and len(names) == len({n.casefold() for n in names}), "duplicate/case-colliding archive entries")
        require(all(safe_relative(name.rstrip("/")) and (name == TOP + "/" or name.startswith(TOP + "/")) for name in names),
                "ZIP must contain only the whole root Signal2M.vst3 bundle")
        payload, links, modes = {}, {}, {}
        for entry in archive.infolist():
            if entry.is_dir():
                continue
            name = entry.filename[len(TOP) + 1:]
            mode = entry.external_attr >> 16
            modes[name] = stat.S_IMODE(mode)
            if stat.S_ISLNK(mode):
                target = archive.read(entry).decode("utf-8", errors="strict")
                resolved = os.path.normpath(str(PurePosixPath(name).parent / target)).replace("\\", "/")
                require(not target.startswith("/") and ":" not in target and "\\" not in target
                        and "\0" not in target and not resolved.startswith("../") and resolved != "..", "escaping archive symlink")
                links[name] = target
            else:
                require(stat.S_ISREG(mode), "archive contains a non-regular special file")
                payload[name] = archive.read(entry)
        manifest_checks(payload, links, modes)
        require(payload == bundle_files, "full archived bundle differs from the actual checked bundle")
        actual_links = {p.relative_to(bundle).as_posix(): os.readlink(p) for p in bundle.rglob("*") if p.is_symlink()}
        require(links == actual_links, "archive symlinks differ from the actual checked bundle")
        if sys.platform == "darwin":
            actual_modes = {p.relative_to(bundle).as_posix(): stat.S_IMODE(p.lstat().st_mode)
                            for p in bundle.rglob("*") if p.is_file() or p.is_symlink()}
            require(modes == actual_modes, "archive modes differ from native checked bundle")
        require(modes.get("Contents/MacOS/Signal2M", 0) & 0o111, "archive lost executable permissions")
        if not quiet:
            passed("bundle-only ZIP, CRC/SHA, full bundle bytes, links/modes and scoped embedded manifest")
            print("ARCHIVE_SHA256 " + sha(path.read_bytes()))


def self_test() -> None:
    def thin(cpu: int, minimum: tuple[int, int, int], dependency: str = "/usr/lib/libSystem.B.dylib") -> bytes:
        encoded = dependency.encode("utf-8") + b"\0"
        length = (24 + len(encoded) + 7) & ~7
        dylib = struct.pack("<6I", 0xC, length, 24, 0, 0, 0) + encoded
        dylib += bytes(length - len(dylib))
        packed_minimum = (minimum[0] << 16) | (minimum[1] << 8) | minimum[2]
        build = struct.pack("<6I", 0x32, 24, 1, packed_minimum, 0x0F0000, 0)
        signature_offset = 32 + len(dylib) + len(build) + 16
        signature = struct.pack("<4I", 0x1D, 16, signature_offset, 12)
        subtype = 3 if cpu == 0x01000007 else 0
        header = struct.pack("<8I", 0xFEEDFACF, cpu, subtype, 8, 3, len(dylib + build + signature), 0, 0)
        return header + dylib + build + signature + b"\xfa\xde\x0c\xc0" + bytes(8)

    def fat(first: bytes, second: bytes, *, wide: bool = False, little: bool = False) -> bytes:
        endian = "<" if little else ">"
        table_end = 72 if wide else 48
        a = (table_end + 15) & ~15
        b = (a + len(first) + 15) & ~15
        entry = "IIQQII" if wide else "5I"
        extra = (0,) if wide else ()
        return (struct.pack(endian + "2I", 0xCAFEBABF if wide else 0xCAFEBABE, 2)
                + struct.pack(endian + entry, 0x01000007, 3, a, len(first), 4, *extra)
                + struct.pack(endian + entry, 0x0100000C, 0, b, len(second), 4, *extra)
                + bytes(a - table_end) + first + bytes(b - a - len(first)) + second)

    intel = thin(0x01000007, (10, 15, 0))
    arm = thin(0x0100000C, (11, 0, 0))
    valid = fat(intel, arm)
    for wide in (False, True):
        for little in (False, True):
            require(len(universal_macho(fat(intel, arm, wide=wide, little=little))) == 2,
                    "self-test rejected valid generated universal structure")
    bad_cases = {"PE file": b"MZ" + bytes(256), "single slice": intel,
                 "truncated table": valid[:24], "truncated slice": valid[:-4],
                 "wrong minimum OS": fat(thin(0x01000007, (13, 0, 0)), arm),
                 "external runtime": fat(thin(0x01000007, (10, 15, 0), "/usr/local/lib/libextra.dylib"), arm),
                 "Windows runtime": fat(thin(0x01000007, (10, 15, 0), "/usr/lib/vcruntime140.dll"), arm),
                 "fat/thin CPU mismatch": fat(intel, intel)}
    invalid_command = bytearray(intel)
    struct.pack_into("<I", invalid_command, 36, 0)
    bad_cases["zero-sized load command"] = fat(bytes(invalid_command), arm)
    invalid_signature = intel[:-12] + b"BAD!" + intel[-8:]
    bad_cases["missing signature superblob"] = fat(invalid_signature, arm)
    invalid_signature_range = bytearray(intel)
    struct.pack_into("<I", invalid_signature_range, len(intel) - 16, 0x7FFFFFFF)
    bad_cases["out-of-range signature blob"] = fat(bytes(invalid_signature_range), arm)
    overlap = bytearray(valid)
    struct.pack_into(">I", overlap, 36, 64)
    bad_cases["overlapping slices"] = bytes(overlap)
    for name, data in bad_cases.items():
        try:
            universal_macho(data)
        except (ValueError, UnicodeError):
            continue
        raise ValueError("self-test accepted " + name)
    require(sdk_json('{"URL":"https://example.test/,]", "A":[1,],}') == {"URL": "https://example.test/,]", "A": [1]}, "trailing-comma parser damaged string")
    require(not any(safe_relative(name) for name in ("../escape", "/absolute", "C:/x", "x\\y")), "unsafe path accepted")
    # Generated archive fixtures exercise delivery shape and the deliberate
    # signature/executable exclusions. These are NOT macOS build artifacts.
    import tempfile
    native = {"platform": "macOS", "version": VERSION, "release_channel": "release", "show_beta": False,
              "native_test_architecture": "arm64", "universal_binary_architectures": ["arm64", "x86_64"],
              "ctest": "passed", "tests": sorted(EXPECTED_TESTS), "mac_update_gid": "640439888",
              "donation_gid": "1525027926", "source_tested_executable_sha256": "0" * 64}
    payload = {"Contents/MacOS/Signal2M": b"synthetic archive fixture, not Mach-O",
               "Contents/_CodeSignature/CodeResources": b"synthetic seal fixture",
               "Contents/Resources/Legal/THIRD_PARTY_NOTICES_MACOS.md": b"synthetic notices",
               "Contents/Resources/Legal/licenses/example.txt": b"synthetic license",
               NATIVE_PATH: json.dumps(native).encode("utf-8"),
               PROVENANCE_PATH: json.dumps({"version": VERSION, "release_channel": "release", "show_beta": False,
                                            "source_files": {"Source/example.cpp": "1" * 64}}).encode("utf-8")}
    modes = {name: 0o755 if name == "Contents/MacOS/Signal2M" else 0o644 for name in payload}
    manifest = {"version": VERSION, "product": HOST_NAME, "format": "VST3", "platform": "macOS Universal",
                "architectures": ["arm64", "x86_64"], "bundle_id": "sound.lmz.signal2m",
                "inventory_exclusions": EXCLUSIONS, "native_checks": native,
                "source_tested_executable_sha256": "0" * 64, "symlinks": {},
                "files": {name: sha(data) for name, data in payload.items() if not inventory_excluded(name)},
                "modes": {name: mode for name, mode in modes.items() if not inventory_excluded(name)}}
    manifest.update(schema_version=2, release_channel="release", show_beta=False,
                    provenance=json.loads(payload[PROVENANCE_PATH].decode("utf-8")),
                    hash_scope="bundle-relative pre-sign resource inventory; final executable, signature and manifest are excluded to avoid circular hashes",
                    signing_policy={"kind": "ad-hoc", "notarization": "not-performed", "verification_required_before_publish": True},
                    delivery_validation_policy={"native_architecture": "arm64", "required": ["full-zip-byte-mode-link-match", "extracted-codesign", "extracted-native-vst3-host", "independent-package-review"]})
    payload[MANIFEST_PATH] = json.dumps(manifest).encode("utf-8")
    modes[MANIFEST_PATH] = 0o644
    with tempfile.TemporaryDirectory(prefix="signal2m-reviewer-zip-") as temporary:
        bundle = Path(temporary) / TOP
        for name, data in payload.items():
            file = bundle / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_bytes(data)
            file.chmod(modes[name])
        archive_path = Path(temporary) / "fixture.zip"
        def archive_case(changed=None, *, extra=None, parent="", executable_mode=0o755):
            entries = dict(payload)
            if changed:
                entries.update(changed)
            with zipfile.ZipFile(archive_path, "w") as archive:
                for name, data in entries.items():
                    entry = zipfile.ZipInfo(parent + TOP + "/" + name)
                    entry.create_system = 3
                    mode = executable_mode if name == "Contents/MacOS/Signal2M" else modes[name]
                    entry.external_attr = (stat.S_IFREG | mode) << 16
                    archive.writestr(entry, data)
                if extra:
                    entry = zipfile.ZipInfo(extra)
                    entry.create_system = 3
                    entry.external_attr = (stat.S_IFREG | 0o644) << 16
                    archive.writestr(entry, b"outside-bundle")
            archive_path.with_suffix(".zip.sha256").write_text(sha(archive_path.read_bytes()) + "\n", encoding="utf-8")
        archive_case()
        zip_checks(archive_path, payload, bundle, quiet=True)
        negative_archives = [
            {"parent": "old-delivery-folder/"}, {"extra": "README.txt"},
            {"extra": "__MACOSX/._Signal2M.vst3"}, {"executable_mode": 0o644},
            {"changed": {"Contents/MacOS/Signal2M": b"tampered excluded executable"}},
            {"changed": {"Contents/_CodeSignature/CodeResources": b"tampered excluded seal"}},
            {"changed": {"Contents/Resources/Legal/licenses/example.txt": b"tampered inventoried resource"}},
        ]
        for kwargs in negative_archives:
            archive_case(**kwargs)
            try:
                zip_checks(archive_path, payload, bundle, quiet=True)
            except ValueError:
                continue
            raise ValueError("self-test accepted invalid bundle-only archive: " + str(kwargs))
    print(f"RESULT reviewer_macho_parser_self_test=PASS negative_cases={len(bad_cases)} archive_negative_cases={len(negative_archives)} native_mac_build=NOT_RUN")


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="strict")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path)
    parser.add_argument("--zip", type=Path)
    parser.add_argument("--offline", action="store_true", help="structural checks only; no native signature approval")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        require(args.bundle is None and args.zip is None, "self-test must not be mixed with delivery approval")
        self_test()
        return 0
    require(args.bundle is not None, "--bundle is required for a real artifact")
    files = bundle_checks(args.bundle.resolve(), args.offline)
    if args.zip:
        zip_checks(args.zip.resolve(), files, args.bundle.resolve())
    print("RESULT structural_checks=PASS native_signature=" + ("PENDING" if args.offline else "PASS") + " native_vst3_loading=SEPARATE_CHECK_REQUIRED")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, TypeError, struct.error, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        print("ERROR " + str(error), file=sys.stderr)
        raise SystemExit(1)
