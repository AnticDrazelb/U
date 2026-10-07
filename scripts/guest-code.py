# Disassembles the game code in a crash report: crash.log's "guest code <address>: <8 words>"
# lines (ExceptionHandler_posix.cpp: around the crashed function's LR and its callers' return
# addresses) into a minimal big-endian PPC ELF, then llvm-objdump.
#   python3 guest-code.py crash-block.txt
import re
import struct
import subprocess
import sys
import tempfile

words = {}
for line in open(sys.argv[1], errors="replace"):
    m = re.match(r"guest code ([0-9a-f]{8}): ((?:[0-9a-f]{8} ?){8})", line.strip())
    if m:
        base = int(m.group(1), 16)
        for i, w in enumerate(m.group(2).split()):
            words[base + 4 * i] = int(w, 16)
if not words:
    sys.exit()

# contiguous blocks
blocks = []
for address in sorted(words):
    if blocks and blocks[-1][0] + 4 * len(blocks[-1][1]) == address:
        blocks[-1][1].append(words[address])
    else:
        blocks.append([address, [words[address]]])

names = b"\0.shstrtab\0" + b"".join(b".t%d\0" % i for i in range(len(blocks)))
body, offsets = b"", []
for address, ws in blocks:
    offsets.append(0x34 + len(body))
    body += struct.pack(">%dI" % len(ws), *ws)
strtab = 0x34 + len(body)
body += names + b"\0" * (-len(names) % 4)
shoff = 0x34 + len(body)
sections = [struct.pack(">10I", *([0] * 10)), struct.pack(">10I", 1, 3, 0, 0, strtab, len(names), 0, 0, 1, 0)]
name_at = len(b"\0.shstrtab\0")
for i, (address, ws) in enumerate(blocks):
    sections.append(struct.pack(">10I", name_at, 1, 6, address, offsets[i], 4 * len(ws), 0, 0, 4, 0))
    name_at += len(b".t%d\0" % i)
header = b"\x7fELF\x01\x02\x01" + b"\0" * 9 + struct.pack(">HHIIIIIHHHHHH", 2, 20, 1, 0, 0, shoff, 0, 0x34, 0, 0, 40, len(sections), 1)
with tempfile.NamedTemporaryFile(suffix=".elf", delete=False) as f:
    f.write(header + body + b"".join(sections))
out = subprocess.run(["llvm-objdump-21", "-d", "--triple=powerpc", f.name], capture_output=True, text=True).stdout
print("\n".join(l for l in out.splitlines() if re.match(r"\s*[0-9a-f]+:", l) or l.startswith("Disassembly")))
