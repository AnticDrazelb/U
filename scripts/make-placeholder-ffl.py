# Writes placeholder Mii parts resources (FFLResHigh.dat, FFLResMiddle.dat and their LG variants)
# for consoles without a dump of the Wii U's Mii resource title (0005001B10056000). Games link the
# Mii library (FFL) statically; when it can't open its resource, NSMBU dereferences a null model
# later and crashes. These files hold no Nintendo data: every 3D part is empty (FFL draws nothing
# for an empty shape) and every texture part is one blank 64x64 texture (a texture can't be empty:
# FFL reads its footer from the end of the data). Miis are then invisible instead of crashing the game.
# Layout from the FFL decompilation (github.com/aboood40091/ffl, tools/FFLResource.py).
#   python make-placeholder-ffl.py <output content folder>
import struct
import sys
from pathlib import Path

FS_IO_BUFFER_ALIGN = 0x40
STRATEGY_UNCOMPRESSED = 5
# texture parts types (beard, cap, eye, eyebrow, faceline, face makeup, glass, mole, mouth,
# mustache, noseline) and shape parts types (beard, cap 1/2, faceline, glass, mask, noseline,
# nose, hair 1/2, forehead 1/2): number of parts of each
TEXTURE_PARTS = [3, 132, 62, 24, 12, 12, 9, 2, 37, 6, 18]
SHAPE_PARTS = [4, 132, 132, 12, 1, 12, 18, 18, 132, 132, 132, 132]
HEADER_SIZE = 0x4A00

# the blank texture: 64x64 RGBA8 (texture format 2), one mip level, transparent; image data then
# the footer (mip offset, width, height, mip count, format)
WIDTH = HEIGHT = 64
texture = bytes(WIDTH * HEIGHT * 4) + struct.pack(">I2H2B2x", 0, WIDTH, HEIGHT, 1, 2)
texture_pos = HEADER_SIZE  # 0x40-aligned, as parts data must be


def parts_info(pos, size):
    # data position, size, compressed size, compress level, window bits, memory level, strategy
    return struct.pack(">3I4B", pos, size, size, 0, 0, 0, STRATEGY_UNCOMPRESSED)


texture_header = struct.pack(">%dI" % len(TEXTURE_PARTS), *([len(texture)] * len(TEXTURE_PARTS)))
texture_header += b"".join(parts_info(texture_pos, len(texture)) * n for n in TEXTURE_PARTS)
assert len(texture_header) == 0x13FC
shape_header = bytes(4 * len(SHAPE_PARTS)) + bytes(0x10 * sum(SHAPE_PARTS))  # all parts empty
assert len(shape_header) == 0x35C0

# magic, version, uncompress buffer size (unused: nothing is compressed), an unused word,
# "is expand" (no: parts are read from the file one by one), texture header, shape header
header = struct.pack(">4s2I4xI", b"FFRA", 0x00070000, len(texture), 0) + texture_header + shape_header
header += bytes(HEADER_SIZE - len(header))
assert len(header) == HEADER_SIZE

data = header + texture
data += bytes(-len(data) % FS_IO_BUFFER_ALIGN)  # FFL reads parts in 0x40-byte rounded sizes

out = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
out.mkdir(parents=True, exist_ok=True)
for name in ("FFLResHigh.dat", "FFLResMiddle.dat", "FFLResHighLG.dat", "FFLResMiddleLG.dat"):
    (out / name).write_bytes(data)
    print("wrote", out / name, len(data), "bytes")
