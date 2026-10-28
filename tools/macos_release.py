"""Shared, read-only release metadata for the macOS recipe. UTF-8 text."""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
MAC_UPDATE_GID = "640439888"
DONATION_GID = "1525027926"
EXPECTED_TESTS = frozenset({
    "Signal2MDspBehavior", "Signal2MMidiNoteVoice", "Signal2MIndependentDsp",
    "Signal2MParametersAndState", "Signal2MManualUpdateChecker", "Signal2MDonationLinks",
    "Signal2MRealMacVst3Module", "Signal2MMacUpdatePolicy", "Signal2MMacPackageParser",
    "Signal2MRealMacUpdateLifecycle",
})


def project_version(root: Path = ROOT) -> str:
    source = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    versions = re.findall(r"(?im)^\s*project\s*\(\s*Signal2M\s+VERSION\s+(\d+\.\d+\.\d+)\b", source)
    if len(versions) != 1:
        raise ValueError("CMakeLists.txt must declare one numeric Signal2M project version")
    return versions[0]


def require_release_cache(path: Path) -> None:
    entries = re.findall(r"(?m)^SIGNAL2M_SHOW_BETA:([^=\r\n]+)=([^\r\n]*)$", path.read_text(encoding="utf-8"))
    if entries != [("BOOL", "OFF")]:
        raise ValueError("CMake must configure SIGNAL2M_SHOW_BETA as BOOL=OFF for the Mac release")


def source_provenance(root: Path = ROOT) -> dict:
    commit = os.environ.get("GITHUB_SHA")
    if not commit and (root / ".git").exists():
        result = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], check=False,
                                capture_output=True, text=True, encoding="utf-8", errors="strict")
        if result.returncode == 0:
            commit = result.stdout.strip()
    if commit and not re.fullmatch(r"[0-9a-fA-F]{40}", commit):
        raise ValueError("Invalid source commit in release provenance")
    paths = [root / "CMakeLists.txt", *sorted((root / "Source").rglob("*"))]
    paths += [root / "tools" / name for name in ("macos_release.py", "build_macos.py", "package_macos.py")]
    hashes = {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
              for path in paths if path.is_file()}
    repository, run_id = os.environ.get("GITHUB_REPOSITORY"), os.environ.get("GITHUB_RUN_ID")
    return {
        "version": project_version(root), "release_channel": "release", "show_beta": False,
        "source_commit": commit, "source_files": hashes,
        "github_run_url": f"https://github.com/{repository}/actions/runs/{run_id}" if repository and run_id else None,
    }
