#!/usr/bin/env python3
"""The render target and stencil test's screen (xbox-core --png): python3 check.py screen.png"""
import sys

from PIL import Image

img = Image.open(sys.argv[1]).convert("RGB")
failures = 0
def check(what, at, expected):
    global failures
    got = img.getpixel(at)
    ok = all(abs(g - e) <= 4 for g, e in zip(got, expected))
    print(f"{'PASS' if ok else 'FAIL'} {what} {at}: {got} (expected {expected})")
    failures += not ok

check("stencil: red inside A", (110, 240), (255, 0, 0))
check("stencil: red inside A, near its corner", (42, 162), (255, 0, 0))
check("stencil: nothing outside A", (250, 240), (0, 0, 0))
check("stencil: nothing outside A, above it", (110, 150), (0, 0, 0))
check("render target: its green half", (400, 240), (0, 255, 0))
check("render target: its blue half", (490, 240), (0, 0, 255))
check("render target: the edge between, green side", (442, 200), (0, 255, 0))
check("render target: the edge between, blue side", (446, 200), (0, 0, 255))
print(f"render target and stencil test: {failures} failures")
sys.exit(1 if failures else 0)
