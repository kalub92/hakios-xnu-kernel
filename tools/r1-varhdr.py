#!/usr/bin/env python3
"""r1-varhdr.py LBA BLOCKS > varhdr.bin -- sector 0 of the GPT partition xnudata, the header
bsd/dev/redwood_emmc.c checks before it writes (LBA, BLOCKS: the GPT entry's first sector and length,
512-byte sectors from the eMMC's start). Little-endian: "XNUVAR01", u64 lba, u64 blocks, u32 zlib crc32
of those 24 bytes, zeros to 512. Write it only through /dev/block/by-name/xnudata.
Staying clear of userdata is the caller's business (the kernel checks the GPT); this refuses super.
r1-varhdr.py test: the self-check."""
import struct, sys, zlib

SUPER = (1605632, 1605632 + 17172000)       # GPT super, [first, end)


def header(lba, blocks):
    if lba < 34 or blocks < 4096:
        raise ValueError("before the GPT's usable blocks, or under 2 MB")
    if lba < SUPER[1] and SUPER[0] < lba + blocks:
        raise ValueError("overlaps super")
    head = struct.pack("<8sQQ", b"XNUVAR01", lba, blocks)
    return (head + struct.pack("<I", zlib.crc32(head))).ljust(512, b"\0")


if sys.argv[1:] == ["test"]:
    h = header(52396032, 191760320)
    if len(h) != 512 or struct.unpack_from("<I", h, 24)[0] != zlib.crc32(h[:24]):
        raise SystemExit("bad header")
    for bad in ((33, 8192), (52396032, 4095), (SUPER[1] - 1, 8192), (SUPER[0] - 4096, 4097), (9262616, 8192)):
        try:
            header(*bad)
        except ValueError:
            continue
        raise SystemExit(f"accepted {bad}")
    print("ok")
else:
    sys.stdout.buffer.write(header(int(sys.argv[1]), int(sys.argv[2])))
