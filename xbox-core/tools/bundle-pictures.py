#!/usr/bin/env python3
"""The pictures in a log bundle (xbox-core-logs.txt: picture-N.png sections in base64), written out
as PNG files: python3 tools/bundle-pictures.py xbox-core-logs.txt [out folder]"""
import base64
import os
import re
import sys

text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
out = sys.argv[2] if len(sys.argv) > 2 else "."
os.makedirs(out, exist_ok=True)
for m in re.finditer(r"===== (\S+\.png) \((\d+) bytes, base64([^\n]*)=====\n((?:[A-Za-z0-9+/=]+\n)+)", text):
    data = base64.b64decode(m.group(4).replace("\n", ""))
    path = os.path.join(out, m.group(1))
    open(path, "wb").write(data)
    print(f"{path}: {len(data)} bytes{m.group(3).rstrip()}")
