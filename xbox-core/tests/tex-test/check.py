#!/usr/bin/env python3
"""The texture stage test's screen (xbox-core --png): python3 check.py screen.png"""
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

faces = [("+X", (255, 0, 0)), ("-X", (0, 255, 0)), ("+Y", (0, 0, 255)), ("-Y", (255, 255, 0)),
         ("+Z", (255, 0, 255)), ("-Z", (0, 255, 255))]
for f, (name, colour) in enumerate(faces):
    check(f"cube map face {name}", (75 + f * 95, 110), colour)
check("bump environment: the lookup moved to green", (100, 320), (0, 255, 0))
check("bump environment: a zero matrix keeps red", (240, 320), (255, 0, 0))
check("clip plane: the right half drawn", (560, 320), (255, 255, 255))
check("clip plane: the left half discarded", (400, 320), (0, 0, 0))
print(f"texture stage test: {failures} failures")
sys.exit(1 if failures else 0)
