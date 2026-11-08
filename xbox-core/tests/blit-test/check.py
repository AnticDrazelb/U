#!/usr/bin/env python3
"""The blit test's screen (xbox-core --png): the 100x80 rectangle at (300, 200) must equal the one at
(20, 20), and hold the pattern's colours. python3 check.py screen.png"""
import sys

from PIL import Image, ImageChops

img = Image.open(sys.argv[1]).convert("RGB")
source = img.crop((20, 20, 120, 100))
dest = img.crop((300, 200, 400, 280))
same = ImageChops.difference(source, dest).getbbox() is None
colours = [dest.getpixel((10, 10)), dest.getpixel((60, 10)), dest.getpixel((10, 70)), dest.getpixel((60, 70)),
           dest.getpixel((50, 32))]
expected = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0), (255, 255, 255)]
failures = 0
for what, ok in [("the copy equals its source", same), ("the copy's colours", colours == expected),
                 ("nothing outside it", img.getpixel((299, 199)) == (0, 0, 0) and img.getpixel((400, 280)) == (0, 0, 0))]:
    print(("PASS " if ok else "FAIL ") + what)
    failures += not ok
print(f"blit test: {failures} failures")
sys.exit(1 if failures else 0)
