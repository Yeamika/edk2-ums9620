#!/bin/bash
# Fetch selected upstream sources and build a private copy of BaseTools.
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")" && pwd -P)
output= edk2=
source_args=()
while (($#)); do
  case "$1" in
    --output) output=${2:?}; shift 2;;
    --edk2) edk2=${2:?}; shift 2;;
    --source-lock|--source-cache|--source-repo|--source-ref) source_args+=("$1" "${2:?}"); shift 2;;
    --help|-h) echo 'Usage: build-tools.sh [--output] DIR [--edk2 DIR] [--source-cache DIR] [--source-lock FILE] [--source-repo NAME=URL] [--source-ref NAME=REV]'; exit 0;;
    -*) echo "Unknown option: $1" >&2; exit 2;;
    *) test -z "$output" || { echo 'Output specified twice' >&2; exit 2; }; output=$1; shift;;
  esac
done
output=$(realpath -m "${output:?pass --output DIR}")
case "$output/" in "$repo/"*) echo 'BaseTools output must be outside the source tree' >&2; exit 2;; esac
test ! -e "$output" || { echo 'Choose a fresh BaseTools output directory' >&2; exit 2; }
edk2_args=()
if [[ -n $edk2 ]]; then edk2_args=(--edk2 "$edk2"); fi
python3 "$repo/tools/upstream.py" prepare --output "$output/upstream" "${edk2_args[@]}" "${source_args[@]}"
edk2=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["edk2"])' "$output/upstream/source-paths.json")
python3 - "$edk2/BaseTools" "$output" <<'PY'
import shutil, sys
shutil.copytree(sys.argv[1], sys.argv[2], dirs_exist_ok=True,
                ignore=shutil.ignore_patterns('.git', '__pycache__'))
PY
make -C "$output" -j "${PJB_JOBS:-4}"
printf 'EDK_TOOLS_PATH=%s\n' "$output"
