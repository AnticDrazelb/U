# Extracts the code of a Wii U RPX/RPL (ELF32 big-endian, sections zlib-compressed when flagged
# 0x08000000) into a minimal ELF that llvm-objdump can disassemble at the game's addresses:
#   python rpx-text.py Minecraft.Client.rpx out.elf
#   llvm-objdump-21 -d --triple=powerpc --start-address=0x2d87600 --stop-address=0x2d87700 out.elf
import struct
import sys
import zlib

data = open(sys.argv[1], "rb").read()
shoff, = struct.unpack_from(">I", data, 0x20)
shentsize, shnum = struct.unpack_from(">HH", data, 0x2E)
code = []
for i in range(shnum):
    name, stype, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(">10I", data, shoff + i * shentsize)
    if stype != 1 or not (flags & 0x4) or size == 0:  # PROGBITS + EXECINSTR
        continue
    raw = data[off:off + size]
    if flags & 0x08000000:
        raw = zlib.decompress(raw[4:])
    code.append((addr, raw))
    print("code section at %#x, %#x bytes" % (addr, len(raw)))

# a minimal ELF32 BE (PPC) with one executable section per code section
names = b"\0.shstrtab\0" + b"".join(b".text%d\0" % i for i in range(len(code)))
body = b""
offsets = []
at = 0x34
for addr, raw in code:
    offsets.append(at + len(body))
    body += raw
    body += b"\0" * (-len(body) % 4)
strtab_off = at + len(body)
body += names
body += b"\0" * (-len(body) % 4)
sh_off = at + len(body)
sections = [struct.pack(">10I", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
sections.append(struct.pack(">10I", 1, 3, 0, 0, strtab_off, len(names), 0, 0, 1, 0))
name_at = len(b"\0.shstrtab\0")
for i, (addr, raw) in enumerate(code):
    sections.append(struct.pack(">10I", name_at, 1, 0x6, addr, offsets[i], len(raw), 0, 0, 4, 0))
    name_at += len(b".text%d\0" % i)
header = b"\x7fELF\x01\x02\x01" + b"\0" * 9 + struct.pack(">HHIIIIIHHHHHH", 2, 20, 1, 0, 0, sh_off, 0, 0x34, 0, 0, 40, len(sections), 1)
open(sys.argv[2], "wb").write(header + body + b"".join(sections))
