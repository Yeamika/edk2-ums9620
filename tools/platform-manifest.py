#!/usr/bin/env python3
"""Record or check the inputs and files of a completed platform build."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import subprocess

ROOT = Path(__file__).resolve().parents[1]
DEFAULTS = {'W202_STORAGE_TEST': 'FALSE', 'W202_BOARD_SERVICES': 'TRUE',
            'W202_ANDROID_BOOT_TEST': 'FALSE', 'W202_ANDROID_BOOT_ENTER': 'FALSE',
            'W202_LK_RETURN_TEST': 'FALSE', 'W202_LK_RETURN_PROBE_ONLY': 'FALSE',
            'W202_TOUCH_TEST': 'TRUE', 'W202_TOUCH_DIRECT': 'FALSE', 'W202_TOUCH_RELOAD': 'FALSE'}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=('create', 'verify'))
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--abi-include', type=Path, required=True)
    p.add_argument('--touch-firmware', type=Path)
    a = p.parse_args()
    build = a.build.resolve()
    source = {str(f.relative_to(ROOT)): sha(f) for f in sorted((ROOT/'Ums9620Pkg').rglob('*'))
              if f.is_file() and f.suffix != '.md'}
    directory = build/'workspace/Build/Ums9620/RELEASE_GCC5'
    files = [f for f in sorted((directory/'FV').rglob('*')) if f.is_file()]
    files.extend(sorted((directory/'AARCH64').glob('*.efi')))
    if not files or not (directory/'FV/UMS9620.fd').is_file():
        raise SystemExit('Missing platform build artifacts')
    record = {'schema': 1, 'source_files': source,
              'abi_header_sha256': sha(a.abi_include/'pocketjs_boot_context.h'),
              'touch_firmware_sha256': sha(a.touch_firmware) if a.touch_firmware else None,
              'defines': {key: os.environ.get(key, default) for key, default in DEFAULTS.items()},
              'files': {str(f.relative_to(build)): sha(f) for f in files}}
    if (build/'upstream.lock.json').is_file():
        api = runpy.run_path(str(ROOT/'tools/upstream.py'))
        upstream = json.loads((build/'upstream.lock.json').read_text())
        paths = json.loads((build/'source-paths.json').read_text())
        actual, _ = api['inspect'](Path(paths['edk2']))
        if api['identity'](actual) != api['identity'](upstream):
            raise SystemExit('Platform source checkout differs from the resolved upstream lock')
        record.update(schema=2, edk2=upstream['dependencies']['edk2']['commit'], upstream_sources=upstream)
    else:
        record['edk2'] = subprocess.check_output(
            ['git', '-C', str(ROOT/'edk2'), 'rev-parse', 'HEAD'], text=True).strip()
    manifest = build/'platform-build.json'
    if a.action == 'create':
        manifest.write_text(json.dumps(record, indent=2)+'\n')
    elif json.loads(manifest.read_text()) != record:
        raise SystemExit('Platform build differs from the current source, profile, ABI or recorded artifacts')
    print(f'Platform manifest {a.action}: passed')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error
