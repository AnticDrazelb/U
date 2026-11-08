#!/usr/bin/env python3
"""Checks the fixed-function test's screen (a PNG from xbox-core --png) against values worked out by
hand: python3 check.py screen.png"""
import math
import sys

from PIL import Image

img = Image.open(sys.argv[1]).convert("RGB")
failures = 0


def check(what, at, expected, tolerance=3):
    global failures
    got = img.getpixel(at)
    ok = all(abs(g - e) <= tolerance for g, e in zip(got, expected))
    print(f"{'PASS' if ok else 'FAIL'} {what}: {got} (expected {expected})")
    failures += not ok


# unlit: the corners' colours (red, green, blue, white from top left, clockwise)
check("unlit top left", (21, 141), (255, 0, 0), 8)
check("unlit top right", (198, 141), (0, 255, 0), 8)
check("unlit bottom right", (198, 338), (0, 0, 255), 8)
check("unlit bottom left", (21, 338), (255, 255, 255), 8)
# lit: ambient 0.1 + diffuse (1, 0.5, 0.25) * N.L 1 = (1.1 -> 1, 0.6, 0.35)
for at in [(240, 150), (320, 240), (400, 330)]:
    check(f"lit {at}", at, (255, round(0.6 * 255), round(0.35 * 255)))
# a point light at (530, 240, -60), the normal towards it: N.L = 60 / distance, per vertex
check("under the light", (530, 240), (255, 255, 255), 4)
d = math.sqrt(90 ** 2 + 100 ** 2 + 60 ** 2)
v = round(60 / d * 255)
check("the corner (440, 140)", (440, 140), (v, v, v), 4)
print(f"fixed-function test: {failures} failures")
sys.exit(1 if failures else 0)
