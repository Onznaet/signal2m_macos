"""Independent read-only NSIS payload/resource/script checks. Never runs setup.

7-Zip reads each member to stdout; no extraction into installation directories.
Windows resource APIs map the EXE as data only, never as executable code.
"""
from __future__ import annotations
import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
import zipfile

from ReviewerPackageChecks import ROOT, VERSION, HOST_NAME, ZIP, VERIFIED_BINARY_SHA256, sha

INSTALLER = ROOT / "dist" / f"LMZ-Signal-2M-v{VERSION}-Setup.exe"
NSIS_ARCHIVE_SHA256 = "ba63dffc4410ee89193e1cb5a41989991bd77c61068da17e3156d136b7b0b3d8"


def check(value: bool, description: str) -> None:
    print(("PASS " if value else "FAIL ") + description, flush=True)
    if not value:
        raise AssertionError(description)


def run7z(tool: Path, *args: str) -> subprocess.CompletedProcess:
    result = subprocess.run([str(tool), *args, "-sccUTF-8"], capture_output=True, timeout=30)
    if result.returncode != 0:
        raise RuntimeError("7-Zip read failed: " + result.stderr.decode("utf-8", errors="strict"))
    return result


def version_strings(path: Path) -> dict[str, str]:
    api = ctypes.WinDLL("version", use_last_error=True)
    api.GetFileVersionInfoSizeW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
    api.GetFileVersionInfoSizeW.restype = wintypes.DWORD
    api.GetFileVersionInfoW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
    api.GetFileVersionInfoW.restype = wintypes.BOOL
    api.VerQueryValueW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.UINT)]
    api.VerQueryValueW.restype = wintypes.BOOL
    ignored = wintypes.DWORD()
    size = api.GetFileVersionInfoSizeW(str(path), ctypes.byref(ignored))
    check(size > 0, "installer has a Windows version resource")
    block = ctypes.create_string_buffer(size)
    check(bool(api.GetFileVersionInfoW(str(path), 0, size, block)), "read installer version data without executing setup")
    pointer, length = ctypes.c_void_p(), wintypes.UINT()
    check(bool(api.VerQueryValueW(block, "\\VarFileInfo\\Translation", ctypes.byref(pointer), ctypes.byref(length))),
          "installer declares resource language/codepage")
    translations = ctypes.string_at(pointer, length.value)
    result = {}
    for language, codepage in struct.iter_unpack("<HH", translations):
        for field in ("ProductName", "ProductVersion", "FileVersion", "FileDescription"):
            query = f"\\StringFileInfo\\{language:04x}{codepage:04x}\\{field}"
            if api.VerQueryValueW(block, query, ctypes.byref(pointer), ctypes.byref(length)):
                result[field] = ctypes.wstring_at(pointer.value).rstrip("\0")
    return result


def execution_manifest(path: Path) -> bytes:
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.LoadLibraryExW.argtypes = [wintypes.LPCWSTR, ctypes.c_void_p, wintypes.DWORD]
    kernel.LoadLibraryExW.restype = ctypes.c_void_p
    kernel.FindResourceW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    kernel.FindResourceW.restype = ctypes.c_void_p
    kernel.SizeofResource.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel.SizeofResource.restype = wintypes.DWORD
    kernel.LoadResource.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel.LoadResource.restype = ctypes.c_void_p
    kernel.LockResource.argtypes = [ctypes.c_void_p]
    kernel.LockResource.restype = ctypes.c_void_p
    kernel.FreeLibrary.argtypes = [ctypes.c_void_p]
    kernel.FreeLibrary.restype = wintypes.BOOL
    # LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE: no entry point.
    module = kernel.LoadLibraryExW(str(path), None, 0x02 | 0x20)
    check(bool(module), "map installer strictly as data/resources")
    try:
        resource = kernel.FindResourceW(module, ctypes.c_void_p(1), ctypes.c_void_p(24))
        check(bool(resource), "installer has an embedded application manifest")
        size = kernel.SizeofResource(module, resource)
        loaded = kernel.LoadResource(module, resource)
        pointer = kernel.LockResource(loaded)
        check(bool(pointer) and size > 0, "read application manifest bytes without running installer")
        return ctypes.string_at(pointer, size)
    finally:
        kernel.FreeLibrary(module)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sevenzip", type=Path, default=Path(r"C:\Program Files\7-Zip\7z.exe"))
    args = parser.parse_args()
    check(args.sevenzip.is_file(), "read-only NSIS archive reader is available")
    check(INSTALLER.is_file() and ZIP.is_file(), "shipping installer and matching version ZIP exist")
    executable = INSTALLER.read_bytes()
    check(executable[:2] == b"MZ", "shipping installer has a PE header")
    offset = struct.unpack_from("<I", executable, 0x3C)[0]
    machine = struct.unpack_from("<H", executable, offset + 4)[0]
    check(executable[offset:offset + 4] == b"PE\0\0" and machine in (0x14C, 0x8664),
          "installer bootstrap is a valid Windows PE (x86 NSIS stub may install x64 payload)")
    check(INSTALLER.with_suffix(".exe.sha256").read_text(encoding="utf-8").split()[0] == sha(executable),
          "external installer SHA-256 matches actual EXE")
    resources = version_strings(INSTALLER)
    check(resources.get("ProductName") == HOST_NAME and resources.get("ProductVersion") == VERSION
          and resources.get("FileVersion") == VERSION, "shipping installer is branded LMZ Signal 2M with version 0.4.0")
    check(HOST_NAME in resources.get("FileDescription", ""), "installer description identifies the product")
    manifest = ET.fromstring(execution_manifest(INSTALLER).decode("utf-8-sig"))
    levels = [element.attrib.get("level") for element in manifest.iter() if element.tag.endswith("requestedExecutionLevel")]
    check(levels == ["requireAdministrator"], "shipping EXE has admin manifest, not the test-only user-manifest variant")

    script = (ROOT / "installer/Signal2M.nsi").read_text(encoding="utf-8")
    check('InstallDir "$COMMONFILES64\\VST3"' in script and '!insertmacro MUI_PAGE_DIRECTORY' in script,
          "default destination is 64-bit Common Files VST3 and the user can choose another parent")
    check('${RunningX64}' in script and 'Abort' in script, "installer script gates unsupported Windows architecture")
    check('MUI_PAGE_WELCOME' in script and 'LMZ — Ленинградский модульный звук' in script,
          "welcome page and LMZ branding are explicitly configured")
    check('Call DetectAction' in script and '"Обновить"' in script and '"Установить"' in script,
          "install/update action uses the selected existing module path")
    check('CreateFileW' in script and 'i 0x40000000, i 0, p 0, i 3,' in script,
          "existing-module lock preflight uses OPEN_EXISTING without writing or truncating")
    check(not re.search(r'^\s*(?:Delete|RMDir|WriteReg\w*|DeleteReg\w*|WriteUninstaller|Exec(?:Wait|Shell)?|nsExec::\w+)\b',
                        script, re.MULTILINE | re.IGNORECASE)
          and 'MUI_FINISHPAGE_RUN' not in script,
          "shipping script has no delete, registry, uninstaller or automatic program-launch operation")

    listing = run7z(args.sevenzip, "l", "-slt", str(INSTALLER)).stdout.decode("utf-8")
    check("Type = Nsis" in listing, "EXE is readable as a NSIS archive")
    body = listing.replace("\r\n", "\n").split("----------", 1)[1]
    entries: dict[str, str] = {}
    for block in body.split("\n\n"):
        values = dict(line.split(" = ", 1) for line in block.splitlines() if " = " in line)
        if "Path" not in values or values.get("Folder") == "+":
            continue
        name = values["Path"].replace("\\", "/")
        check(not PurePosixPath(name).is_absolute() and not PureWindowsPath(name).is_absolute()
              and ".." not in PurePosixPath(name).parts and ":" not in name, "safe embedded relative path: " + name)
        check(name not in entries, "unique embedded file: " + name)
        entries[name] = values["Path"]
    def member(name: str) -> bytes:
        return run7z(args.sevenzip, "x", "-so", "-bd", "-bso0", "-bsp0", "-spd", str(INSTALLER), entries[name]).stdout

    with zipfile.ZipFile(ZIP) as archive:
        expected: dict[str, bytes] = {}
        prefix = "Signal2M.vst3/Contents/Resources/Documentation/"
        for name in archive.namelist():
            if name.startswith("Signal2M.vst3/"):
                expected[name] = archive.read(name)
            elif name in ("INSTALL_RU.txt", "VERSION_NOTES_RU.txt", "THIRD_PARTY_NOTICES.md"):
                expected[prefix + name] = archive.read(name)
            elif name.startswith("licenses/"):
                expected[prefix + name] = archive.read(name)
        runtime = {"$PLUGINSDIR/modern-wizard.bmp", "$PLUGINSDIR/nsDialogs.dll", "$PLUGINSDIR/System.dll"}
        check(set(entries) == set(expected) | runtime,
              f"installer has exactly {len(expected)} reviewed product files and three NSIS runtime resources")
        for name, original in expected.items():
            extracted = member(name)
            check(extracted == original, "byte-exact installer payload: " + name)
            if name.endswith("Contents/x86_64-win/Signal2M.vst3"):
                check(sha(extracted) == VERIFIED_BINARY_SHA256, "installer contains the exact independently loaded VST3 x64 module")
        check(member("$PLUGINSDIR/modern-wizard.bmp") == (ROOT / "installer/assets/lmz-welcome.bmp").read_bytes(),
              "installer embeds the reviewed LMZ welcome artwork")
    nsis_zip = ROOT / ".tools/nsis-3.13.zip"
    check(sha(nsis_zip.read_bytes()) == NSIS_ARCHIVE_SHA256, "pinned official NSIS tool archive remains unchanged")
    with zipfile.ZipFile(nsis_zip) as compiler:
        for name in ("System.dll", "nsDialogs.dll"):
            check(member("$PLUGINSDIR/" + name) == compiler.read("nsis-3.13/Plugins/x86-unicode/" + name),
                  "original pinned NSIS runtime helper: " + name)
    build = json.loads((ROOT / "build/installer-build.json").read_text(encoding="utf-8"))
    check(build.get("version") == VERSION and build.get("sha256") == sha(executable)
          and build.get("payload_module_sha256") == VERIFIED_BINARY_SHA256
          and build.get("system_install_performed") is False, "builder provenance matches independently inspected EXE and module")
    print(f"RESULT installer_readonly_checks=PASS payload_files={len(expected)} archive_files={len(entries)} bytes={len(executable)}")
    print("INSTALLER_SHA256 " + sha(executable))
    print("MODULE_SHA256 " + VERIFIED_BINARY_SHA256)
    return 0


if __name__ == "__main__":
    for output in (sys.stdout, sys.stderr):
        if hasattr(output, "reconfigure"):
            output.reconfigure(encoding="utf-8", errors="strict")
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, ValueError, KeyError, IndexError, RuntimeError, subprocess.TimeoutExpired) as error:
        print("ERROR " + str(error), file=sys.stderr)
        raise SystemExit(1)
