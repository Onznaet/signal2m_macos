#!/usr/bin/env python3
"""Workspace-only NSIS smoke tests. Never launches the production installer.

The same .nsi is compiled with SIGNAL2M_INSTALLER_SMOKE, whose only change is
RequestExecutionLevel user. The resulting asInvoker executable and every /D=
destination must resolve strictly beneath ROOT/build. No recursive deletion.
"""

from __future__ import annotations

import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import time
import uuid
import xml.etree.ElementTree as ET
import zipfile


ROOT = Path(__file__).resolve().parents[1]
BUILD = (ROOT / "build").resolve()
SCRIPT = ROOT / "installer/Signal2M.nsi"
COMPILER = ROOT / ".tools/nsis-3.13/Bin/makensis.exe"
MODULE = "Signal2M.vst3/Contents/x86_64-win/Signal2M.vst3"
DOCS = {"INSTALL_RU.txt", "VERSION_NOTES_RU.txt", "THIRD_PARTY_NOTICES.md"}
DOC_ROOT = "Signal2M.vst3/Contents/Resources/Documentation"


class CheckFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CheckFailure(message)


def inside_build(path: Path) -> Path:
    require(BUILD != ROOT and BUILD.is_relative_to(ROOT), "ROOT/build must itself resolve inside the workspace")
    resolved = path.resolve()
    require(resolved != BUILD and resolved.is_relative_to(BUILD), f"Refusing a path outside ROOT/build: {resolved}")
    return resolved


def write_bytes(path: Path, data: bytes) -> None:
    path = inside_build(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_digest(path: Path) -> str:
    return digest(path.read_bytes())


class Checks:
    def __init__(self) -> None:
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]
        self.folder = inside_build(BUILD / "installer-smoke" / stamp)
        self.folder.mkdir(parents=True, exist_ok=False)
        self.report_path = inside_build(BUILD / "tests" / f"installer-smoke-{stamp}.json")
        self.log_path = self.report_path.with_suffix(".log")
        self.lines: list[str] = []
        self.report = {
            "test_only": True,
            "manifest_expected": "asInvoker",
            "system_install_performed": False,
            "production_installer_executed": False,
            "recursive_deletion_performed": False,
            "workspace": str(ROOT),
            "run_folder": str(self.folder),
            "processes": [],
            "checks": [],
        }
        self.temp = inside_build(self.folder / "nsis-temp")
        self.temp.mkdir()

    def log(self, message: str) -> None:
        self.lines.append(message)
        print(message, flush=True)

    def passed(self, name: str, details: dict) -> None:
        self.report["checks"].append({"name": name, "passed": True, **details})
        self.log("PASS " + name)

    def save(self) -> None:
        self.report_path.parent.mkdir(parents=True, exist_ok=True)
        self.report_path.write_text(json.dumps(self.report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
        self.log_path.write_text("\n".join(self.lines) + "\n", encoding="utf-8", newline="\n")

    def run(self, command: list[str] | str, *, executable: Path | None = None, timeout: int = 45) -> subprocess.CompletedProcess:
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0  # SW_HIDE; /S separately suppresses installer UI.
        environment = os.environ.copy()
        environment["TEMP"] = str(self.temp)
        environment["TMP"] = str(self.temp)
        environment["PYTHONUTF8"] = "1"
        environment["PYTHONIOENCODING"] = "utf-8"
        display = command if isinstance(command, str) else subprocess.list2cmdline(command)
        self.log("COMMAND " + display)
        start = time.monotonic()
        result = subprocess.run(command, executable=str(executable) if executable else None,
                                cwd=SCRIPT.parent, env=environment, shell=False,
                                creationflags=subprocess.CREATE_NO_WINDOW, startupinfo=startup,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, encoding="utf-8", errors="strict", timeout=timeout)
        self.report["processes"].append({"command": display, "exit_code": result.returncode,
                                          "seconds": time.monotonic() - start,
                                          "hidden": True, "create_no_window": True})
        if result.stdout:
            self.log(result.stdout.rstrip())
        return result


def read_verified_payload(checks: Checks) -> tuple[str, Path, dict[str, bytes]]:
    source = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\(Signal2M VERSION (\d+\.\d+\.\d+)\b", source)
    require(match is not None, "Numeric build version not found")
    version = match.group(1)
    archive_path = ROOT / "dist" / f"Signal2M-v{version}-Windows-x64.zip"
    expected_archive = archive_path.with_suffix(".zip.sha256").read_text(encoding="utf-8").split()[0]
    require(file_digest(archive_path) == expected_archive, "Delivery ZIP SHA-256 mismatch")
    payload = inside_build(checks.folder / "payload")
    expected_install: dict[str, bytes] = {}
    with zipfile.ZipFile(archive_path) as archive:
        require(archive.testzip() is None, "Delivery ZIP CRC failure")
        manifest = json.loads(archive.read("MANIFEST.json").decode("utf-8"))
        require(manifest["version"] == version and manifest["product"] == "LMZ Signal 2M", "Package/build identity mismatch")
        require(set(archive.namelist()) == set(manifest["files"]) | {"MANIFEST.json"}, "Package file set mismatch")
        require(len({name.casefold() for name in manifest["files"]}) == len(manifest["files"]), "Case-colliding ZIP entries")
        for name, expected_hash in manifest["files"].items():
            relative = PurePosixPath(name)
            require(not relative.is_absolute() and ".." not in relative.parts and "\\" not in name and ":" not in name,
                    "Unsafe ZIP path: " + name)
            target = inside_build(payload / Path(*relative.parts))
            require(target.is_relative_to(payload), "ZIP entry escapes staged payload")
            data = archive.read(name)
            require(digest(data) == expected_hash, "Package member hash mismatch: " + name)
            write_bytes(target, data)
            if name.startswith("Signal2M.vst3/"):
                expected_install[name] = data
            elif name in DOCS or name.startswith("licenses/"):
                expected_install[DOC_ROOT + "/" + name] = data
    license_count = sum(name.startswith(DOC_ROOT + "/licenses/") for name in expected_install)
    require(license_count == 32, f"Expected 32 installed license files, found {license_count}")
    require(all(DOC_ROOT + "/" + name in expected_install for name in DOCS), "Missing one of the three installed documents")
    require(MODULE in expected_install, "The full VST3 module is missing from the payload")
    checks.report.update({"version": version, "source_zip": str(archive_path), "source_zip_sha256": expected_archive,
                          "nsi_sha256": file_digest(SCRIPT), "nsis_version": "3.13",
                          "expected_installed_files": len(expected_install), "license_files": license_count,
                          "document_files": len(DOCS), "expected_hashes": {name: digest(data) for name, data in expected_install.items()}})
    return version, payload, expected_install


def manifest_level(executable: Path) -> tuple[str, bytes]:
    executable = inside_build(executable)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.LoadLibraryExW.argtypes = [wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD]
    kernel.LoadLibraryExW.restype = wintypes.HMODULE
    kernel.FindResourceW.argtypes = [wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p]
    kernel.FindResourceW.restype = wintypes.HANDLE
    kernel.SizeofResource.argtypes = [wintypes.HMODULE, wintypes.HANDLE]
    kernel.SizeofResource.restype = wintypes.DWORD
    kernel.LoadResource.argtypes = [wintypes.HMODULE, wintypes.HANDLE]
    kernel.LoadResource.restype = wintypes.HANDLE
    kernel.LockResource.argtypes = [wintypes.HANDLE]
    kernel.LockResource.restype = ctypes.c_void_p
    kernel.FreeLibrary.argtypes = [wintypes.HMODULE]
    kernel.FreeLibrary.restype = wintypes.BOOL
    # Map data/resources only. This does not initialize or execute the EXE.
    module = kernel.LoadLibraryExW(str(executable), None, 0x00000002 | 0x00000020)
    if not module:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        resource = kernel.FindResourceW(module, ctypes.c_void_p(1), ctypes.c_void_p(24))  # RT_MANIFEST
        require(bool(resource), "Test executable has no manifest resource")
        size = kernel.SizeofResource(module, resource)
        loaded = kernel.LoadResource(module, resource)
        pointer = kernel.LockResource(loaded)
        require(bool(pointer) and size > 0, "Cannot read test executable manifest")
        data = ctypes.string_at(pointer, size)
        root = ET.fromstring(data.decode("utf-8-sig").rstrip("\0"))
        levels = [item.attrib.get("level", "") for item in root.iter()
                  if item.tag.rsplit("}", 1)[-1] == "requestedExecutionLevel"]
        require(len(levels) == 1, "Ambiguous requestedExecutionLevel")
        return levels[0], data
    finally:
        kernel.FreeLibrary(module)


def compile_smoke(checks: Checks, version: str, payload: Path) -> Path:
    require(COMPILER.is_file(), "Pinned portable NSIS compiler is missing")
    output = inside_build(checks.folder / f"LMZ-Signal-2M-v{version}-SMOKE-USER.exe")
    command = [str(COMPILER), "/V3", "/INPUTCHARSET", "UTF8", "/OUTPUTCHARSET", "UTF8",
               "/DSIGNAL2M_INSTALLER_SMOKE", f"/DPRODUCT_VERSION={version}",
               f"/DPAYLOAD_DIR={inside_build(payload)}", f"/DOUTPUT_FILE={output}", str(SCRIPT)]
    compiled = checks.run(command, timeout=60)
    require(compiled.returncode == 0 and output.is_file(), "Workspace smoke installer compilation failed")
    level, manifest = manifest_level(output)
    require(level == "asInvoker", "Refusing to run an installer with an elevating/unknown manifest")
    write_bytes(checks.folder / "test-only.manifest.xml", manifest)
    checks.report.update({"test_installer": str(output), "test_installer_sha256": file_digest(output),
                          "test_manifest_level": level, "compile_defines": command[6:-1]})
    checks.passed("same-nsi test-only user manifest", {"level": level, "production_installer_executed": False})
    return output


def run_installer(checks: Checks, executable: Path, destination: Path) -> int:
    executable = inside_build(executable)
    destination = inside_build(destination)
    require(destination.is_relative_to(checks.folder), "Installation destination must be inside the unique smoke folder")
    require(executable.is_relative_to(checks.folder), "Only the newly compiled smoke executable may be launched")
    require('"' not in str(destination), "Quotes are forbidden in NSIS /D= destination")
    # NSIS explicitly requires /D last and UNQUOTED even when the path contains
    # spaces. Passing an argument list would add CRT quotes, so construct the
    # raw command line and specify the executable independently to CreateProcess.
    command = subprocess.list2cmdline([str(executable), "/S"]) + " /D=" + str(destination)
    require(command.endswith(" /D=" + str(destination)), "NSIS destination override is not the final raw parameter")
    return checks.run(command, executable=executable).returncode


def snapshot(folder: Path) -> dict[str, str]:
    folder = inside_build(folder)
    result = {}
    if folder.exists():
        for path in folder.rglob("*"):
            resolved = inside_build(path)
            require(resolved.is_relative_to(folder), "A fixture contains an escaping link")
            if path.is_file():
                result[path.relative_to(folder).as_posix()] = file_digest(path)
    return result


def verify_payload(folder: Path, expected: dict[str, bytes], extras: dict[str, bytes] | None = None) -> dict[str, str]:
    expected_hashes = {name: digest(data) for name, data in expected.items()}
    expected_hashes.update({name: digest(data) for name, data in (extras or {}).items()})
    actual = snapshot(folder)
    require(actual == expected_hashes, "Installed file set or hashes differ from the verified payload and preserved sentinels")
    return actual


class ExclusiveFileLock:
    def __init__(self, path: Path) -> None:
        self.path = inside_build(path)
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                           wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        self.kernel.CreateFileW.restype = wintypes.HANDLE
        self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel.CloseHandle.restype = wintypes.BOOL
        self.handle = None

    def __enter__(self) -> ExclusiveFileLock:
        self.handle = self.kernel.CreateFileW(str(self.path), 0x80000000 | 0x40000000, 0, None, 3, 0, None)
        require(self.handle not in (None, ctypes.c_void_p(-1).value), "Could not acquire exclusive module lock")
        probe = self.kernel.CreateFileW(str(self.path), 0x40000000, 0, None, 3, 0, None)
        error = ctypes.get_last_error()
        if probe not in (None, ctypes.c_void_p(-1).value):
            self.kernel.CloseHandle(probe)
            self.kernel.CloseHandle(self.handle)
            self.handle = None
            raise CheckFailure("Exclusive file-lock fixture did not prevent a second writer")
        require(error == 32, f"Expected ERROR_SHARING_VIOLATION=32, received {error}")
        return self

    def __exit__(self, *_: object) -> None:
        if self.handle not in (None, ctypes.c_void_p(-1).value):
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def run_checks(checks: Checks) -> None:
    # Exercise the path guard without creating anything outside build.
    for forbidden in (ROOT / "dist", ROOT, BUILD, Path(r"C:\Program Files\Common Files\VST3")):
        try:
            inside_build(forbidden)
        except CheckFailure:
            continue
        raise CheckFailure("Workspace path guard accepted an outside/default destination")
    checks.passed("workspace destination guard", {"outside_targets_rejected": 4})
    version, payload, expected = read_verified_payload(checks)
    executable = compile_smoke(checks, version, payload)
    clean = inside_build(checks.folder / "clean target with spaces")
    require(not clean.exists(), "Clean target must not already exist")
    clean_exit = run_installer(checks, executable, clean)
    require(clean_exit == 0, f"Clean silent installation exited {clean_exit}")
    verify_payload(clean, expected)
    checks.passed("clean full-bundle/docs/licenses installation", {"target": str(clean), "exit_code": clean_exit,
                                                                  "verified_files": len(expected), "documents": 3, "licenses": 32})

    extras = {
        "unrelated-sentinel.txt": b"Unrelated parent file must survive.\n",
        "OtherPlugin.vst3/Contents/x86_64-win/OtherPlugin.vst3": b"Other plugin fixture, never loaded.\n",
        "Signal2M.vst3/user-preserved-sentinel.txt": b"An unknown existing bundle file must survive.\n",
    }
    for name, data in extras.items():
        write_bytes(clean / name, data)
    write_bytes(clean / MODULE, expected[MODULE] + b"\nSMOKE-OLD-MODULE-MARKER\n")
    require(file_digest(clean / MODULE) != digest(expected[MODULE]), "Update fixture did not modify the old module")
    update_exit = run_installer(checks, executable, clean)
    require(update_exit == 0, f"Silent update exited {update_exit}")
    verify_payload(clean, expected, extras)
    checks.passed("update restores module and preserves unrelated files", {"exit_code": update_exit,
                                                                            "module_sha256": file_digest(clean / MODULE),
                                                                            "sentinels_preserved": sorted(extras)})

    locked = inside_build(checks.folder / "locked target with spaces")
    # Every expected destination intentionally differs from payload. Hash equality
    # after the failed update therefore detects even one premature metadata/doc
    # copy before the installer notices that the module cannot be opened.
    for index, (name, data) in enumerate(expected.items()):
        marker = f"\nSMOKE-LOCK-PREFLIGHT-CANARY-{index:03d}\n".encode("utf-8")
        write_bytes(locked / name, data + marker)
    for name, data in extras.items():
        write_bytes(locked / name, data)
    before = snapshot(locked)
    with ExclusiveFileLock(locked / MODULE):
        locked_exit = run_installer(checks, executable, locked)
    after = snapshot(locked)
    require(locked_exit != 0, "Installer accepted an exclusively locked existing module")
    require(before == after, "Locked-module preflight changed at least one destination byte or file")
    checks.passed("locked module aborts before any payload mutation", {"exit_code": locked_exit,
                                                                      "share_mode": 0, "lock_error_probe": 32,
                                                                      "distinct_payload_canaries": len(expected),
                                                                      "before_hashes": before, "after_hashes": after})
    require(file_digest(SCRIPT) == checks.report["nsi_sha256"], "Installer script changed during smoke validation")
    checks.report["success"] = True


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    require(os.name == "nt", "Installer smoke checks require Windows")
    checks = Checks()
    try:
        run_checks(checks)
    except Exception as error:
        checks.report["success"] = False
        checks.report["error"] = f"{type(error).__name__}: {error}"
        checks.log("FAIL " + checks.report["error"])
    finally:
        checks.save()
        print("JSON " + str(checks.report_path), flush=True)
        print("LOG " + str(checks.log_path), flush=True)
        print("TEST-ONLY USER MANIFEST; NO SYSTEM INSTALL; FIXTURES RETAINED", flush=True)
    return 0 if checks.report.get("success") else 1


if __name__ == "__main__":
    raise SystemExit(main())
