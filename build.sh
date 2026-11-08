#!/bin/bash
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")" && pwd -P)
output= abi= firmware= edk2=
source_args=()
while (($#)); do
  case "$1" in
    --output) output=${2:?}; shift 2;;
    --abi-include) abi=${2:?}; shift 2;;
    --touch-firmware) firmware=${2:?}; shift 2;;
    --edk2) edk2=${2:?}; shift 2;;
    --source-lock|--source-cache|--source-repo|--source-ref) source_args+=("$1" "${2:?}"); shift 2;;
    --help|-h) echo 'Usage: build.sh --output DIR --abi-include DIR [--touch-firmware FILE] [--edk2 DIR] [--source-cache DIR] [--source-lock FILE] [--source-repo NAME=URL] [--source-ref NAME=COMMIT|BRANCH|TAG|latest]'; exit 0;;
    *) echo "Unknown option: $1" >&2; exit 2;;
  esac
done
: "${output:?pass --output}" "${abi:?pass --abi-include}"
output=$(realpath -m "$output")
case "$output/" in "$repo/"*) echo 'Output must be outside the source tree' >&2; exit 2;; esac
test ! -e "$output" || { echo 'Choose a fresh platform output directory' >&2; exit 2; }
edk2_args=()
if [[ -n $edk2 ]]; then edk2_args=(--edk2 "$edk2"); fi
python3 "$repo/tools/upstream.py" prepare --output "$output" "${edk2_args[@]}" "${source_args[@]}"
edk2=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["edk2"])' "$output/source-paths.json")
if [[ -z ${EDK_TOOLS_PATH:-} ]]; then
  bash "$repo/build-tools.sh" --edk2 "$edk2" --output "$output/basetools"
  export EDK_TOOLS_PATH="$output/basetools"
fi
if [[ -n $firmware ]]; then
  export W202_TOUCH_RELOAD=TRUE W202_TOUCH_FIRMWARE=$firmware
fi
bash "$repo/build-platform.sh" "$edk2" "$output" "$abi"
manifest_args=()
if [[ -n $firmware ]]; then manifest_args=(--touch-firmware "$firmware"); fi
python3 "$repo/tools/platform-manifest.py" create --build "$output" --abi-include "$abi" "${manifest_args[@]}"
