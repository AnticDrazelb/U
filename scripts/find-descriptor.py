# Searches the GPU flight recorder's descriptor dumps (port/mesa/ac_ps4_drm.c: mem-N.bin, records
# of 'DMEM' length va + bytes) for references to a GPU address range: buffer descriptors (address
# in dwords 0-1, 48 bits) and image descriptors (address >> 8 in dwords 0-1, 40 bits).
#   python find-descriptor.py <flight folder> <start va> <end va>
import glob
import struct
import sys

folder, lo, hi = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
for path in sorted(glob.glob(folder + "/mem-*.bin")):
    data = open(path, "rb").read()
    pos = 0
    records = hits = 0
    while pos + 16 <= len(data):
        magic, length, va = struct.unpack_from("<IIQ", data, pos)
        if magic != 0x4D454D44:
            print("%s: bad record at %d" % (path, pos))
            break
        pos += 16
        words = struct.unpack_from("<%dI" % (length // 4), data, pos)
        pos += length
        records += 1
        for j in range(len(words) - 1):
            buffer = words[j] | ((words[j + 1] & 0xFFFF) << 32)
            image = (words[j] | ((words[j + 1] & 0xFF) << 32)) << 8
            for kind, addr in (("buffer", buffer), ("image", image)):
                if lo <= addr < hi:
                    hits += 1
                    context = " ".join("%08x" % w for w in words[max(0, j - 2):j + 8])
                    print("%s: %s %#x at va %#x (+%#x): %s" % (path, kind, addr, va + 4 * j, 4 * j, context))
    print("%s: %d records, %d hits" % (path, records, hits))
