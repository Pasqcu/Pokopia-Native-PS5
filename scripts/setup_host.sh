#!/usr/bin/env bash
# Host setup: a Python 3.10+ virtual environment with the lifter's dependencies, then the pinned third-party components.
#   requirements: git, curl, unzip, cmake >= 3.25, ninja, clang >= 18 (Xcode or LLVM), Python >= 3.10; macOS for the art step
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="${PYTHON:-$(command -v python3.13 || command -v python3.12 || command -v python3.11 || command -v python3)}"
"$PY" -c 'import sys; assert sys.version_info >= (3, 10), "Python 3.10+ needed"'
[[ -x "$ROOT/.venv/bin/python" ]] || "$PY" -m venv "$ROOT/.venv"
"$ROOT/.venv/bin/pip" install --quiet --disable-pip-version-check "capstone==5.0.9" "lz4>=4.3" pyelftools
"$ROOT/.venv/bin/python" -c 'import capstone, lz4; print("python deps ok")'
for t in git curl unzip cmake ninja clang++; do command -v "$t" >/dev/null || { echo "missing: $t" >&2; exit 2; }; done
bash "$ROOT/scripts/fetch_upstream.sh"
