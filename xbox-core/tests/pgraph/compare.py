#!/usr/bin/env python3
"""Compares nxdk_pgraph_tests output from xbox-core with the hardware captures.

    compare.py <our output dir> <golden results dir> [--suite NAME ...] [--worst N] [--html out.html]

Both directories hold <suite>/<test>.png. An image matches when fewer than 1% of its pixels differ
from the console's by more than 24 in any of R, G, B (alpha isn't sent to the TV, so it isn't
compared). Prints each suite's matches and the worst images.
"""
import argparse
import os
import sys

from PIL import Image, ImageChops


def difference(ours_path, golden_path):
    a = Image.open(ours_path).convert("RGB")
    b = Image.open(golden_path).convert("RGB")
    if a.size != b.size:
        return 1.0, "size %dx%d vs %dx%d" % (a.size + b.size)
    diff = ImageChops.difference(a, b)
    # the largest channel difference per pixel, thresholded
    r, g, bl = diff.split()
    worst = ImageChops.lighter(ImageChops.lighter(r, g), bl)
    histogram = worst.histogram()
    bad = sum(histogram[25:])
    return bad / (a.size[0] * a.size[1]), ""


def main():
    p = argparse.ArgumentParser()
    p.add_argument("ours")
    p.add_argument("golden")
    p.add_argument("--suite", action="append", default=[])
    p.add_argument("--worst", type=int, default=25)
    p.add_argument("--threshold", type=float, default=0.01)
    p.add_argument("--include-zeta", action="store_true", help="compare the _ZB depth captures too")
    args = p.parse_args()

    results = []  # (suite, test, fraction, note)
    for suite in sorted(os.listdir(args.ours)):
        directory = os.path.join(args.ours, suite)
        if not os.path.isdir(directory) or (args.suite and suite not in args.suite):
            continue
        for name in sorted(os.listdir(directory)):
            if not name.endswith(".png") or (name.endswith("_ZB.png") and not args.include_zeta):
                continue
            golden = os.path.join(args.golden, suite, name)
            if not os.path.exists(golden):
                continue
            fraction, note = difference(os.path.join(directory, name), golden)
            results.append((suite, name[:-4], fraction, note))

    if not results:
        print("no images to compare")
        return 1
    by_suite = {}
    for suite, _, fraction, _ in results:
        total, good = by_suite.get(suite, (0, 0))
        by_suite[suite] = (total + 1, good + (fraction < args.threshold))
    print("%-40s %s" % ("suite", "match"))
    for suite, (total, good) in sorted(by_suite.items()):
        print("%-40s %d/%d" % (suite, good, total))
    matched = sum(good for _, good in by_suite.values())
    print("\n%d of %d images match (%.1f%%)" % (matched, len(results), 100.0 * matched / len(results)))
    print("\nworst:")
    for suite, test, fraction, note in sorted(results, key=lambda r: -r[2])[: args.worst]:
        print("  %5.1f%%  %s/%s %s" % (100 * fraction, suite, test, note))
    return 0


if __name__ == "__main__":
    sys.exit(main())
