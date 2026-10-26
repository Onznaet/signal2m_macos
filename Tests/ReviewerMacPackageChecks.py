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

VERSION = "0.3.1"
HOST_NAME = "LMZ Signal 2M"
TOP = f"Signal2M-v{VERSION}-macOS-Universal"
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
    passed("Info.plist identity, physical names and 0.3.1 versions")
    executable = bundle / "Contents/MacOS/Signal2M"
    require(executable.is_file() and not executable.is_symlink(), "native executable must be a regular bundle file")
    slices = universal_macho(executable.read_bytes())
    for item in slices:
        require(MAC_FEED in item["bytes"] and WIN_FEED not in item["bytes"], "incorrect platform update feed in " + item["architecture"])
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
    if not offline:
        require(sys.platform == "darwin", "native codesign verification requires macOS; --offline is structural only")
        result = subprocess.run(["/usr/bin/codesign", "--verify", "--strict", "--verbose=2", str(bundle)],
                                capture_output=True, encoding="utf-8", errors="strict", timeout=30)
        require(result.returncode == 0, "native codesign validation failed: " + result.stdout + result.stderr)
        passed("native codesign --verify --strict on the actual bundle")
    print("BINARY_SHA256 " + sha(executable.read_bytes()))
    return regular


def safe_relative(name: str) -> bool:
    parts = PurePosixPath(name).parts
    return bool(parts) and name == PurePosixPath(name).as_posix() and not PurePosixPath(name).is_absolute() and not any(p in ("", ".", "..") for p in parts) and ":" not in name and "\\" not in name and "\0" not in name


def zip_checks(path: Path, bundle_files: dict[str, bytes]) -> None:
    checksum_path = path.with_suffix(path.suffix + ".sha256")
    checksum = checksum_path.read_text(encoding="utf-8").split()[0]
    require(checksum == sha(path.read_bytes()), "archive SHA-256 sidecar mismatch")
    with zipfile.ZipFile(path) as archive:
        require(archive.testzip() is None, "archive CRC failure")
        names = archive.namelist()
        require(len(names) == len(set(names)) and len(names) == len({n.casefold() for n in names}), "duplicate or case-colliding archive entries")
        require(all(safe_relative(n.rstrip("/")) for n in names), "unsafe archive entry path")
        payload = {}
        links = {}
        modes = {}
        for entry in archive.infolist():
            if entry.is_dir():
                continue
            if entry.filename.startswith("__MACOSX/"):
                require(PurePosixPath(entry.filename).name.startswith("._"), "unexpected AppleDouble sidecar")
                continue
            require(entry.filename.startswith(TOP + "/"), "archive has an unexpected top-level directory")
            name = entry.filename[len(TOP) + 1:]
            mode = entry.external_attr >> 16
            modes[name] = stat.S_IMODE(mode)
            if stat.S_ISLNK(mode):
                target = archive.read(entry).decode("utf-8", errors="strict")
                resolved = os.path.normpath(str(PurePosixPath(name).parent / target)).replace("\\", "/")
                require(not target.startswith("/") and ":" not in target and not resolved.startswith("../") and resolved != "..", "escaping archive symlink")
                links[name] = target
            else:
                require(stat.S_ISREG(mode), "archive payload has a non-regular special file")
                payload[name] = archive.read(entry)
        for name in ("MANIFEST.json", "INSTALL_MACOS_RU.txt", "THIRD_PARTY_NOTICES_MACOS.md"):
            require(name in payload, "missing delivery file: " + name)
            require("\ufffd" not in payload[name].decode("utf-8", errors="strict"), "replacement character in delivery text")
        manifest = json.loads(payload["MANIFEST.json"].decode("utf-8"))
        require(manifest.get("version") == VERSION and manifest.get("product") == HOST_NAME and manifest.get("format") == "VST3", "manifest identity mismatch")
        normal = {name: sha(data) for name, data in payload.items() if name != "MANIFEST.json"}
        require(manifest.get("files") == normal and manifest.get("symlinks", {}) == links, "manifest does not account for exact payload bytes/links")
        expected_modes = {name: mode for name, mode in modes.items() if name != "MANIFEST.json"}
        require(manifest.get("modes") == expected_modes, "manifest permissions mismatch")
        archived = {name[len("Signal2M.vst3/"):]: data for name, data in payload.items() if name.startswith("Signal2M.vst3/")}
        require(archived == bundle_files, "archive bundle differs from the actual checked/signature-verified bundle")
        require(modes.get("Signal2M.vst3/Contents/MacOS/Signal2M", 0) & 0o111, "archive lost executable permissions")
        require(not any(n.startswith(("Source/", "Tests/", "build/", ".tools/", "dependencies/")) or n.lower().endswith((".exe", ".dll", ".pdb")) for n in payload), "developer or Windows files leaked into delivery")
        passed("ZIP CRC, SHA-256, paths, executable modes, manifest, and complete checked bundle bytes")
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
    print(f"RESULT reviewer_macho_parser_self_test=PASS negative_cases={len(bad_cases)} native_mac_build=NOT_RUN")


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
        zip_checks(args.zip.resolve(), files)
    print("RESULT structural_checks=PASS native_signature=" + ("PENDING" if args.offline else "PASS") + " native_vst3_loading=SEPARATE_CHECK_REQUIRED")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, TypeError, struct.error, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        print("ERROR " + str(error), file=sys.stderr)
        raise SystemExit(1)
