#!/usr/bin/env python3
"""Build, run native technical checks, and package the macOS Universal VST3."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys

import bootstrap_mac as bootstrap
import macos_release as release


ROOT = bootstrap.ROOT
BUILD = ROOT / "build-macos"


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 4), help="Parallel compilation jobs (default: up to 4)")
    parser.add_argument("--skip-bootstrap", action="store_true", help="Check existing dependencies without downloading or patching")
    parser.add_argument("--mac-update-gid", default=release.MAC_UPDATE_GID, help="Dedicated macOS update tab gid")
    parser.add_argument("--no-package", action="store_true", help="Build and run technical checks only")
    parser.add_argument("--sign-identity", default="-", help="Explicit installed Developer ID Application identity; default '-' is local ad-hoc")
    arguments = parser.parse_args()
    if arguments.jobs < 1 or arguments.jobs > 64:
        parser.error("--jobs must be between 1 and 64")
    if not arguments.mac_update_gid.isascii() or not arguments.mac_update_gid.isdigit():
        parser.error("--mac-update-gid must be numeric")
    try:
        version = release.project_version(ROOT)
        native_arch = bootstrap.check_native_environment()
        bootstrap.ensure_build_tools(arguments.skip_bootstrap)
        bootstrap.ensure_juce(arguments.skip_bootstrap)
        bootstrap.run([
            str(bootstrap.CMAKE), "-S", str(ROOT), "-B", str(BUILD), "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64",
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=10.15", "-DCMAKE_MAKE_PROGRAM:FILEPATH=" + str(bootstrap.NINJA),
            "-DSIGNAL2M_BUILD_TESTS=ON", "-DSIGNAL2M_SHOW_BETA=OFF",
            "-DSIGNAL2M_MAC_UPDATE_GID=" + arguments.mac_update_gid,
            "-DSIGNAL2M_DONATION_GID=" + release.DONATION_GID,
        ])
        release.require_release_cache(BUILD / "CMakeCache.txt")
        bootstrap.run([str(bootstrap.CMAKE), "--build", str(BUILD), "--parallel", str(arguments.jobs)])
        inventory = json.loads(bootstrap.run([str(bootstrap.CTEST), "--test-dir", str(BUILD), "--show-only=json-v1"], capture=True))
        test_names = {test["name"] for test in inventory["tests"]}
        expected_tests = release.EXPECTED_TESTS
        if not expected_tests.issubset(test_names):
            raise bootstrap.BootstrapError("Native test targets are missing: " + ", ".join(sorted(expected_tests - test_names)))
        # arch explicitly selects the native slice. On CI the Intel and Apple
        # Silicon jobs each exercise their own slice; no Rosetta install occurs.
        bootstrap.run([
            "/usr/bin/arch", "-" + native_arch, str(bootstrap.CTEST),
            "--test-dir", str(BUILD), "--output-on-failure", "--timeout", "180",
        ])
        bundle = BUILD / "Signal2M_artefacts/Release/VST3/Signal2M.vst3"
        executable = bundle / "Contents/MacOS/Signal2M"
        result = {
            "platform": "macOS", "native_test_architecture": native_arch,
            "version": version, "release_channel": "release", "show_beta": False, "tests": sorted(test_names),
            "source_tested_executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
            "universal_binary_architectures": ["arm64", "x86_64"],
            "ctest": "passed", "manual_daw_and_visual_checks": "pending",
            "mac_update_gid": arguments.mac_update_gid,
            "donation_gid": release.DONATION_GID,
        }
        (BUILD / "native-test-results.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        if not arguments.no_package:
            bootstrap.run([
                sys.executable, "-X", "utf8", str(ROOT / "tools/package_macos.py"),
                "--bundle", str(bundle), "--sign-identity", arguments.sign_identity,
                "--test-results", str(BUILD / "native-test-results.json"),
            ])
        print("macOS Universal bundle: " + str(bundle))
    except (bootstrap.BootstrapError, OSError, ValueError) as error:
        print("macOS build failed: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
