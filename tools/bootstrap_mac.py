#!/usr/bin/env python3
"""Project-local macOS build dependencies; standard library only, UTF-8."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / ".tools/macos"
PYTHON_TOOLS = TOOLS / "python"
DEPENDENCIES = ROOT / "dependencies"
JUCE = DEPENDENCIES / "JUCE"
JUCE_URL = "https://github.com/juce-framework/JUCE.git"
JUCE_TAG = "7.0.12"
JUCE_COMMIT = "4f43011b96eb0636104cb3e433894cda98243626"
CMAKE_VERSION = "3.31.6"
NINJA_PACKAGE_VERSION = "1.11.1.3"
CMAKE = PYTHON_TOOLS / "cmake/data/bin/cmake"
CTEST = PYTHON_TOOLS / "cmake/data/bin/ctest"
NINJA = PYTHON_TOOLS / "bin/ninja"

# Same verified source compatibility changes as tools/bootstrap.py. This recipe
# never imports or downloads the Windows compiler, runtime, installer or tools.
JUCE_PATCHES = (
    (
        "modules/juce_core/system/juce_StandardHeader.h",
        "#include <typeindex>",
        "#include <typeindex>\n#include <type_traits>",
    ),
    (
        "modules/juce_audio_processors/processors/juce_AudioPluginInstance.h",
        "AudioPluginInstance (const short channelLayoutList[numLayouts][2])",
        "AudioPluginInstance (const short (&channelLayoutList)[numLayouts][2])",
    ),
)


class BootstrapError(RuntimeError):
    pass


def environment() -> dict[str, str]:
    result = os.environ.copy()
    result["PYTHONUTF8"] = "1"
    result["PYTHONIOENCODING"] = "utf-8"
    result["GIT_TERMINAL_PROMPT"] = "0"
    result.pop("GIT_SSL_NO_VERIFY", None)
    return result


def run(command: list[str], *, capture: bool = False) -> str:
    if not capture:
        print("+ " + shlex.join(command), flush=True)
    result = subprocess.run(
        command, cwd=ROOT, env=environment(), check=False,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace",
    )
    if not capture and result.stdout:
        print(result.stdout, end="" if result.stdout.endswith("\n") else "\n", flush=True)
    if result.returncode:
        raise BootstrapError(f"Command failed ({result.returncode}): {shlex.join(command)}\n{result.stdout}")
    return result.stdout


def git(arguments: list[str], *, capture: bool = True) -> str:
    # Leave the native macOS TLS backend in place and explicitly require TLS
    # verification. Nothing is written to system/user Git configuration.
    return run(["git", "-c", "http.sslVerify=true", "-c", "core.quotepath=false", *arguments], capture=capture)


def check_native_environment() -> str:
    if sys.version_info < (3, 9):
        raise BootstrapError("Python 3.9 or newer is required")
    if sys.platform != "darwin" or platform.machine().lower() not in ("arm64", "x86_64"):
        raise BootstrapError("This recipe must run on an Intel or Apple Silicon Mac with Xcode Command Line Tools")
    if shutil.which("git") is None or shutil.which("xcrun") is None:
        raise BootstrapError("Git and xcrun must be available from Xcode Command Line Tools")
    translated = subprocess.run(
        ["/usr/sbin/sysctl", "-in", "sysctl.proc_translated"], env=environment(), check=False,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace",
    )
    if translated.returncode == 0 and translated.stdout.strip() == "1":
        raise BootstrapError("Run this recipe with native arm64 Python on Apple Silicon; this Python is running under Rosetta")
    sdk = run(["xcrun", "--sdk", "macosx", "--show-sdk-path"], capture=True).strip()
    compiler = run(["xcrun", "--sdk", "macosx", "--find", "clang++"], capture=True).strip()
    if not Path(sdk).is_dir() or not Path(compiler).is_file():
        raise BootstrapError("The active developer directory has no usable macOS SDK/clang++; select Xcode/CLT manually")
    print("macOS SDK: " + sdk)
    print("Compiler: " + run([compiler, "--version"], capture=True).splitlines()[0])
    return platform.machine().lower()


def metadata_matches(name: str, version: str) -> bool:
    metadata = PYTHON_TOOLS / f"{name}-{version}.dist-info/METADATA"
    if not metadata.is_file():
        return False
    lines = metadata.read_text(encoding="utf-8").splitlines()
    return f"Name: {name}" in lines and f"Version: {version}" in lines


def ensure_build_tools(check_only: bool) -> None:
    ready = (CMAKE.is_file() and CTEST.is_file() and NINJA.is_file()
             and metadata_matches("cmake", CMAKE_VERSION)
             and metadata_matches("ninja", NINJA_PACKAGE_VERSION))
    if not ready:
        if check_only:
            raise BootstrapError("Pinned macOS CMake/Ninja are missing; run python3 -X utf8 tools/bootstrap_mac.py")
        TOOLS.mkdir(parents=True, exist_ok=True)
        run([
            sys.executable, "-m", "pip", "--isolated", "install",
            "--index-url", "https://pypi.org/simple", "--disable-pip-version-check",
            "--no-cache-dir", "--only-binary=:all:", "--no-deps", "--upgrade",
            "--no-warn-script-location", "--target", str(PYTHON_TOOLS),
            "cmake==" + CMAKE_VERSION, "ninja==" + NINJA_PACKAGE_VERSION,
        ])
    cmake_version = run([str(CMAKE), "--version"], capture=True).splitlines()[0]
    ninja_version = run([str(NINJA), "--version"], capture=True).strip()
    if cmake_version != "cmake version " + CMAKE_VERSION or not ninja_version.startswith("1.11.1"):
        raise BootstrapError("CMake/Ninja executable versions do not match the pinned macOS recipe")
    print(f"CMake {CMAKE_VERSION}; Ninja package {NINJA_PACKAGE_VERSION} ({ninja_version})")


def ensure_juce(check_only: bool) -> None:
    if not JUCE.exists():
        if check_only:
            raise BootstrapError("JUCE is missing; run python3 -X utf8 tools/bootstrap_mac.py")
        DEPENDENCIES.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="signal2m-juce-mac-", dir=DEPENDENCIES) as temporary:
            checkout = Path(temporary) / "JUCE"
            git(["-c", "core.autocrlf=false", "clone", "--branch", JUCE_TAG,
                 "--depth", "1", "--single-branch", JUCE_URL, str(checkout)], capture=False)
            commit = git(["-C", str(checkout), "rev-parse", "HEAD"]).strip()
            if commit != JUCE_COMMIT:
                raise BootstrapError(f"JUCE tag does not match pinned commit: {commit}")
            checkout.rename(JUCE)
    if not (JUCE / ".git").exists():
        raise BootstrapError("JUCE checkout has no Git metadata; the pinned source cannot be verified")
    commit = git(["-C", str(JUCE), "rev-parse", "HEAD"]).strip()
    if commit != JUCE_COMMIT:
        raise BootstrapError(f"JUCE HEAD is {commit}; expected {JUCE_COMMIT}. Existing files were preserved.")
    allowed = {patch[0] for patch in JUCE_PATCHES}
    changed = set(git(["-C", str(JUCE), "diff", "--name-only", "HEAD"]).splitlines())
    untracked = git(["-C", str(JUCE), "ls-files", "--others", "--exclude-standard"]).splitlines()
    if changed - allowed or untracked:
        raise BootstrapError("Unexpected JUCE changes; existing files were preserved: "
                             + ", ".join(sorted(changed - allowed) + untracked))
    updates: list[tuple[Path, str]] = []
    for relative, old, new in JUCE_PATCHES:
        pristine = git(["-C", str(JUCE), "show", f"HEAD:{relative}"])
        if pristine.count(old) != 1:
            raise BootstrapError("JUCE compatibility patch has no unique context: " + relative)
        expected = pristine.replace(old, new, 1)
        path = JUCE / relative
        current = path.read_text(encoding="utf-8")
        if current == expected:
            print("Reuse JUCE compatibility patch: " + relative)
        elif current == pristine:
            if check_only:
                raise BootstrapError("JUCE compatibility patch is missing: " + relative)
            updates.append((path, expected))
        else:
            raise BootstrapError("Unexpected JUCE compatibility file contents: " + relative)
    for path, contents in updates:
        with path.open("w", encoding="utf-8", newline="\n") as destination:
            destination.write(contents)
        print("Apply JUCE compatibility patch: " + str(path.relative_to(JUCE)))
    print(f"JUCE {JUCE_TAG} verified at {JUCE_COMMIT}")


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Read-only dependency/SDK check; no downloads or patches")
    arguments = parser.parse_args()
    try:
        check_native_environment()
        ensure_build_tools(arguments.check)
        ensure_juce(arguments.check)
    except (BootstrapError, OSError) as error:
        print("macOS bootstrap failed: " + str(error), file=sys.stderr)
        return 1
    print("Pinned macOS dependencies are ready. Run bash tools/build_macos.sh.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
