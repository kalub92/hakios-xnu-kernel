#!/usr/bin/env python3
"""Minimal liblp metadata dumper: lpdump.py <super-head.bin>"""
import struct, sys
d = open(sys.argv[1], "rb").read()
g = 4096
magic, ssize = struct.unpack_from("<II", d, g)
assert magic == 0x616C4467, hex(magic)
mmax, slots, lbs = struct.unpack_from("<III", d, g + 40)
print(f"geometry: metadata_max={mmax} slots={slots} block={lbs}")
for s in range(slots):
    o = 4096 * 3 + s * mmax
    if o + 128 > len(d): break
    m, maj, mnr, hsz = struct.unpack_from("<IHHI", d, o)
    if m != 0x414C5030: print(f"slot {s}: no header"); continue
    tsz = struct.unpack_from("<I", d, o + 44)[0]
    pt, ex, gr, bd = [struct.unpack_from("<III", d, o + 80 + 12 * i) for i in range(4)]
    t = o + hsz
    exts = [struct.unpack_from("<QIQI", d, t + ex[0] + i * ex[2]) for i in range(ex[1])]
    groups = [d[t + gr[0] + i * gr[2]: t + gr[0] + i * gr[2] + 36].rstrip(b"\0").decode() for i in range(gr[1])]
    print(f"slot {s} (v{maj}.{mnr}): groups={groups}")
    for i in range(pt[1]):
        po = t + pt[0] + i * pt[2]
        name = d[po:po + 36].rstrip(b"\0").decode()
        attr, fe, ne, gi = struct.unpack_from("<IIII", d, po + 36)
        sectors = sum(exts[fe + k][0] for k in range(ne))
        print(f"   {name:24s} {sectors * 512 / 2**20:9.1f} MiB  extents={ne} group={groups[gi]} attr={attr:#x}")
        for k in range(ne):
            n, ttype, tdata, tsrc = exts[fe + k]
            print(f"      extent {k}: {n} sectors at super sector {tdata} (type {ttype})")
