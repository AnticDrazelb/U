#!/usr/bin/env bash
# Draws the PS4 front end's HOME menu (frontend/home.h) to PNG files on the desktop, from a made-up
# library: a few game folders with generated covers, a disc image on a pretend USB drive, and the
# package's samples' names.
#   tools/ui-preview.sh [output folder]     (default: ~/xc-host/ui-preview; needs build.sh host first)
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$HOME/xc-host/ui-preview}"
LIB="$(mktemp -d)"
trap 'rm -rf "$LIB"' EXIT
mkdir -p "$OUT" "$LIB/games" "$LIB/samples" "$LIB/covers" "$LIB/usb0/games"
for g in "Neon Drift" "Crystal Caverns" "Starfall Tactics" "Harbor Kart Rally" "Moonlit Manor" "Copper Knights"; do
    mkdir -p "$LIB/games/$g"
    : > "$LIB/games/$g/default.xbe"
done
for s in hello triangle mesh sdl_gamecontroller xaudio test-kernel; do : > "$LIB/samples/$s.xbe"; done
head -c 1048576 /dev/zero > "$LIB/usb0/games/Orbital Golf.iso"
mkdir -p "$LIB/usb0/games/Pixel Pioneers" && : > "$LIB/usb0/games/Pixel Pioneers/default.xbe"
# covers for three of them: abstract art, portrait as a case's
python3 - "$LIB" <<'PY'
import sys, random
from PIL import Image, ImageDraw
lib = sys.argv[1]
for name, seed in (("Neon Drift", 3), ("Crystal Caverns", 7), ("Moonlit Manor", 11)):
    random.seed(seed)
    w, h = 600, 850
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    a = [random.randint(20, 120) for _ in range(3)]
    b = [random.randint(120, 255) for _ in range(3)]
    for y in range(h):
        t = y / h
        d.line([(0, y), (w, y)], fill=tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3)))
    for _ in range(14):
        x, y, r = random.randint(0, w), random.randint(0, h), random.randint(30, 160)
        c = tuple(random.randint(60, 255) for _ in range(3))
        d.ellipse([x - r, y - r, x + r, y + r], outline=c, width=random.randint(4, 14))
    d.rectangle([0, 0, w, 70], fill=(18, 120, 40))
    img.save(f"{lib}/games/{name}/cover.jpg", quality=90)
PY
now=$(date +%s)
{
    printf '%s\t7\t%s\tShowed its picture\n' "$LIB/games/Crystal Caverns/default.xbe" "$((now - 1800))"
    printf '%s\t2\t%s\tEnded by itself\n' "$LIB/games/Neon Drift/default.xbe" "$((now - 3 * 86400))"
} > "$LIB/history.txt"
# the last picture shown: Crystal Caverns's (a made-up scene)
python3 - "$LIB/picture-1.png" <<'PY'
import sys, random
from PIL import Image, ImageDraw
random.seed(5)
img = Image.new("RGB", (640, 480))
d = ImageDraw.Draw(img)
for y in range(480):
    t = y / 480
    d.line([(0, y), (640, y)], fill=(int(30 + 60 * t), int(20 + 90 * t), int(70 + 100 * t)))
for _ in range(40):
    x, y = random.randint(0, 640), random.randint(200, 480)
    r = random.randint(10, 60)
    d.polygon([(x, y - r * 2), (x - r, y + r), (x + r, y + r)], fill=(random.randint(80, 200), random.randint(140, 255), 255))
d.rectangle([0, 440, 640, 480], fill=(20, 20, 30))
img.save(sys.argv[1])
PY
"$HOME/xc-host/ui-preview" "$LIB" "$OUT"
