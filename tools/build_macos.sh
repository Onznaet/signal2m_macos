#!/bin/bash
# Run from any directory; all persistent outputs remain inside this project.
set -euo pipefail
export PYTHONUTF8=1
export PYTHONIOENCODING=utf-8
script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec "${PYTHON:-python3}" -X utf8 "$script_dir/build_macos.py" "$@"
