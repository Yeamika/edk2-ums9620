#!/bin/bash
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")" && pwd -P)
output= abi= firmware=
while (($#)); do
  case "$1" in
    --output) output=${2:?}; shift 2;;
    --abi-include) abi=${2:?}; shift 2;;
    --touch-firmware) firmware=${2:?}; shift 2;;
    --help|-h) echo 'Usage: build.sh --output DIR --abi-include DIR [--touch-firmware FILE]'; exit 0;;
    *) echo "Unknown option: $1" >&2; exit 2;;
  esac
done
: "${output:?pass --output}" "${abi:?pass --abi-include}"
output=$(realpath -m "$output")
case "$output/" in "$repo/"*) echo 'Output must be outside the source tree' >&2; exit 2;; esac
python3 - "$repo" <<'PY'
import json, pathlib, subprocess, sys
repo = pathlib.Path(sys.argv[1])
lock = json.loads((repo/'sources.lock.json').read_text())
for name, spec in lock['dependencies'].items():
    actual = subprocess.check_output(['git', '-C', str(repo/name), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != spec['commit']:
        raise SystemExit(f'{name}: expected {spec["commit"]}, found {actual}')
PY
if [[ -z ${EDK_TOOLS_PATH:-} ]]; then
  bash "$repo/build-tools.sh" "$output/basetools"
  export EDK_TOOLS_PATH="$output/basetools"
fi
if [[ -n $firmware ]]; then
  export W202_TOUCH_RELOAD=TRUE W202_TOUCH_FIRMWARE=$firmware
fi
bash "$repo/build-platform.sh" "$repo/edk2" "$output" "$abi"
manifest_args=()
if [[ -n $firmware ]]; then manifest_args=(--touch-firmware "$firmware"); fi
python3 "$repo/tools/platform-manifest.py" create --build "$output" --abi-include "$abi" "${manifest_args[@]}"
