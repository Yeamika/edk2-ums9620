#!/bin/bash
# Diagnostic inherited-LK platform; not a drop-in flash image.
set -euo pipefail
for option in W202_ANDROID_BOOT_TEST W202_LK_RETURN_TEST W202_TOUCH_DIRECT W202_HANDOFF_TEST; do
  if [[ ${!option:-FALSE} == TRUE ]]; then
    echo "$option no longer embeds an application; build and launch the standalone ESP EFI in w202ds-boot" >&2
    exit 2
  fi
done
PJB_EDK=$(realpath "${1:?EDK II checkout}")
PJB_PLATFORM=$(cd -- "$(dirname -- "$0")" && pwd -P)
PJB_WORK=$(realpath -m "${2:?private build directory}")
PJB_BOOT_ABI_INCLUDE=$(realpath "${3:-${PJB_BOOT_ABI_INCLUDE:?pass the public PocketJS ABI include directory}}")
test -f "$PJB_BOOT_ABI_INCLUDE/pocketjs_boot_context.h" || {
  printf '%s\n' 'Missing public pocketjs_boot_context.h' >&2
  exit 2
}
: "${EDK_TOOLS_PATH:?set EDK_TOOLS_PATH to compiled BaseTools}"
touch_args=()
if [[ ${W202_TOUCH_RELOAD:-FALSE} == TRUE ]]; then
  touch_firmware=$(realpath "${W202_TOUCH_FIRMWARE:?explicit guoxian firmware input required}")
  python3 - "$touch_firmware" <<'PY'
import hashlib, pathlib, sys
b = pathlib.Path(sys.argv[1]).read_bytes()
if len(b) != 261120 or hashlib.sha256(b).hexdigest() != 'bdcd327d48628dd703b1d0a8ea778ef7f7aacd7beb227efdb78ea788928f0156':
    raise SystemExit('HX83102J reload requires the pinned guoxian firmware')
PY
  touch_args=(-D "W202_TOUCH_FIRMWARE=$touch_firmware")
fi
export WORKSPACE="$PJB_WORK/workspace" CONF_PATH="$PJB_WORK/workspace/Conf"
export PACKAGES_PATH="$PJB_PLATFORM:$PJB_EDK" GCC5_AARCH64_PREFIX=aarch64-linux-gnu- PYTHON_COMMAND=python3
mkdir -p "$CONF_PATH"
cd "$WORKSPACE"
set --
set +u
source "$PJB_EDK/edksetup.sh"
set -u
build -a AARCH64 -t GCC5 -b RELEASE -p Ums9620Pkg/Ums9620.dsc \
  -D "PJB_BOOT_ABI_INCLUDE=$PJB_BOOT_ABI_INCLUDE" \
  -D "W202_STORAGE_TEST=${W202_STORAGE_TEST:-FALSE}" \
  -D "W202_BOARD_SERVICES=${W202_BOARD_SERVICES:-TRUE}" \
  -D "W202_LK_FASTBOOT=${W202_LK_FASTBOOT:-FALSE}" \
  -D "W202_ANDROID_BOOT_TEST=${W202_ANDROID_BOOT_TEST:-FALSE}" \
  -D "W202_ANDROID_BOOT_ENTER=${W202_ANDROID_BOOT_ENTER:-FALSE}" \
  -D "W202_LK_RETURN_TEST=${W202_LK_RETURN_TEST:-FALSE}" \
  -D "W202_LK_RETURN_PROBE_ONLY=${W202_LK_RETURN_PROBE_ONLY:-FALSE}" \
  -D "W202_TOUCH_TEST=${W202_TOUCH_TEST:-TRUE}" \
  -D "W202_TOUCH_DIRECT=${W202_TOUCH_DIRECT:-FALSE}" \
  -D "W202_TOUCH_RELOAD=${W202_TOUCH_RELOAD:-FALSE}" \
  "${touch_args[@]}" -n "${PJB_JOBS:-4}"
