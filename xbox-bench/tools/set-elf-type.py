#!/usr/bin/env python3
"""Sets the ELF type of the program inside a PS4 fake SELF (eboot.bin) or an OELF, in place.

    set-elf-type.py <eboot.bin|.oelf> <type>     type: exec (0xfe00), exec-aslr (0xfe10) or a number

create-fself always writes ET_SCE_EXEC_ASLR (0xfe10). The system places such a program at
0x400000. ET_SCE_EXEC (0xfe00) is a program at fixed addresses, the ones it was linked at
(xbox-bench's XB_IMAGE_BASE); whether the system loads one there is what the bench's variants
find out. Only the two type bytes of the ELF header change.
"""
import struct
import sys

TYPES = {"exec": 0xFE00, "exec-aslr": 0xFE10}


def main():
    path, kind = sys.argv[1], sys.argv[2]
    new_type = TYPES.get(kind) or int(kind, 0)
    data = bytearray(open(path, "rb").read())
    # the ELF header: the first "\x7fELF" of a 64-bit little-endian file whose type is an SCE one
    at = 0
    while True:
        at = data.find(b"\x7fELF\x02\x01", at)
        if at < 0:
            sys.exit(f"{path}: no SCE ELF header found")
        (old_type,) = struct.unpack_from("<H", data, at + 16)
        if old_type in (0xFE00, 0xFE10):
            break
        at += 4
    struct.pack_into("<H", data, at + 16, new_type)
    open(path, "wb").write(data)
    print(f"{path}: ELF header at {at:#x}, type {old_type:#x} -> {new_type:#x}")


if __name__ == "__main__":
    main()
