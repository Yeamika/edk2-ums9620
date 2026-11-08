#!/bin/bash
# Build a private copy of BaseTools, leaving the pinned submodule clean.
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")" && pwd -P)
output=$(realpath -m "${1:?usage: build-tools.sh OUTPUT_DIRECTORY}")
case "$output/" in "$repo/"*) echo 'BaseTools output must be outside the source tree' >&2; exit 2;; esac
test ! -e "$output" || { echo 'Choose a fresh BaseTools output directory' >&2; exit 2; }
test -f "$repo/edk2/BaseTools/Makefile" || { echo 'Initialize the edk2 submodule first' >&2; exit 2; }
mkdir -p -- "$(dirname -- "$output")"
python3 - "$repo/edk2/BaseTools" "$output" <<'PY'
import shutil, sys
shutil.copytree(sys.argv[1], sys.argv[2], ignore=shutil.ignore_patterns('.git', '__pycache__'))
PY
make -C "$output" -j "${PJB_JOBS:-4}"
printf 'EDK_TOOLS_PATH=%s\n' "$output"
