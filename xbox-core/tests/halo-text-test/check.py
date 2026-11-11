#!/usr/bin/env python3
"""Checks the Halo text test's screen (a PNG from xbox-core --png): python3 check.py screen.png"""
import sys

from PIL import Image

img = Image.open(sys.argv[1]).convert("RGB")
failures = 0


def check(what, at, expected, tolerance=8):
    global failures
    got = img.getpixel(at)
    ok = all(abs(g - e) <= tolerance for g, e in zip(got, expected))
    print(f"{'PASS' if ok else 'FAIL'} {what} {at}: {got} (expected {expected})")
    failures += not ok


for at in [(249, 360), (298, 389), (347, 418)]:
    check("red quad", at, (255, 0, 0))
for at in [(41, 41), (90, 70), (139, 99)]:
    check("green quad", at, (0, 255, 0))
for at in [(246, 357), (350, 421), (298, 356), (298, 422), (38, 38), (142, 102), (5, 5), (320, 240)]:
    check("black", at, (0, 0, 0))
print(f"Halo text test: {failures} failures")
sys.exit(1 if failures else 0)
