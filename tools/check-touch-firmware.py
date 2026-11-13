#!/usr/bin/env python3
"""Validate the supported board touchscreen firmware before platform compilation."""
import hashlib
import json
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
lock = json.loads((root/'Ums9620Pkg/W202ds/firmware/touch/manifest.json').read_text())
data = Path(sys.argv[1]).read_bytes()
if len(data) != lock['bytes'] or hashlib.sha256(data).hexdigest() != lock['sha256']:
    raise SystemExit('Incorrect touchscreen firmware: size or SHA-256 differs')
print('Touchscreen firmware: verified')
