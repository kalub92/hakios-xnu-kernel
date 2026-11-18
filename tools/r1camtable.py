#!/usr/bin/env python3
"""
Make xnu/files/bsd/dev/redwood_r1cam_hi846.h, the Hi846's register tables for redwood_r1cam.c, from Linux's
drivers/media/i2c/hi846.c (GPL-2.0; https://raw.githubusercontent.com/torvalds/linux/master/drivers/media/i2c/hi846.c):
the 4-lane init list, the 1632x1224 mode and its 4-lane MIPI list, in the order hi846_start_streaming writes them.
Like the other camera's tables it is made on your machine and is not in git (.gitignore); without it the driver
says so and does nothing.

  tools/r1camtable.py path/to/hi846.c [out.h] [path/to/hi846_mipi_raw_Sensor-4lane.c]

With the third argument, the r1's own vendor driver (GPL-2.0; github.com/rabbit-hmi-oss/android_kernel_rabbit_mt6765, branch r1,
drivers/misc/mediatek/imgsensor/src/common/v1/hi846_mipi_raw/hi846_mipi_raw_Sensor-4lane.c), its init list and its 3264x2448
30 frames/s list are added (r1cam_hi846_vinit, r1cam_hi846_full; R1CAM_HI846_FULL): the driver's boot-arg r1cam_mode=1.
Linux has no table for that size.
"""
import re
import sys

src = open(sys.argv[1]).read()
out = sys.argv[2] if len(sys.argv) > 2 else "xnu/files/bsd/dev/redwood_r1cam_hi846.h"
names = {m.group(1): int(m.group(2), 16) for m in re.finditer(r"#define\s+(HI846_REG_\w+)\s+(0x[0-9a-fA-F]+)", src)}


def table(name):
    body = re.search(r"static const struct hi846_reg %s\[\] = \{(.*?)\n\};" % name, src, re.S).group(1)
    return [(names[a] if a in names else int(a, 16), int(v, 16)) for a, v in re.findall(r"\{\s*(\w+),\s*(0x[0-9a-fA-F]+)\s*\}", body)]


lists = [("init", "hi846_init_4lane"), ("mode", "mode_1632x1224_config"), ("mipi", "mode_1632x1224_mipi_4lane")]
with open(out, "w") as f:
    f.write("/* made by tools/r1camtable.py from Linux drivers/media/i2c/hi846.c: do not edit, do not commit */\n")
    f.write("struct r1cam_reg { uint16_t reg, val; };\n")
    total = 0
    for short, name in lists:
        t = table(name)
        assert t, name
        total += len(t)
        f.write("static const struct r1cam_reg r1cam_hi846_%s[] = {\t/* %s */\n" % (short, name))
        for i in range(0, len(t), 6):
            f.write("\t" + " ".join("{ 0x%04x, 0x%04x }," % p for p in t[i:i + 6]) + "\n")
        f.write("};\n")
if len(sys.argv) > 3:
    vendor = open(sys.argv[3]).read()

    def pairs(name):
        body = re.search(r"kal_uint16 %s\[\] = \{(.*?)\};" % name, vendor, re.S).group(1)
        body = re.sub(r"//.*|/\*.*?\*/", "", body, flags=re.S)
        v = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", body)]
        assert v and len(v) % 2 == 0, name
        return list(zip(v[0::2], v[1::2]))

    full = dict(pairs("addr_data_pair_capture_30fps_hi846"))
    # the check: 3264x2448, and the frame and line lengths the driver's timing says (30 frames/s)
    assert (full[0x0A12], full[0x0A14], full[0x0006], full[0x0008]) == (3264, 2448, 2526, 3800), full
    with open(out, "a") as f:
        f.write("#define R1CAM_HI846_FULL 1\n")
        for short, name in (("vinit", "addr_data_pair_init_hi846"), ("full", "addr_data_pair_capture_30fps_hi846")):
            t = pairs(name)
            total += len(t)
            f.write("static const struct r1cam_reg r1cam_hi846_%s[] = {\t/* the vendor's %s */\n" % (short, name))
            for i in range(0, len(t), 6):
                f.write("\t" + " ".join("{ 0x%04x, 0x%04x }," % p for p in t[i:i + 6]) + "\n")
            f.write("};\n")
mode = dict(table("mode_1632x1224_config"))
# the check: the mode is the one the driver's geometry says (0x0a12/0x0a14 the output size, 0x0006 the frame length, 0x0008 the line length)
assert (mode[0x0A12], mode[0x0A14], mode[0x0006], mode[0x0008]) == (1632, 1224, 2526, 3800), mode
print("%s: %d writes" % (out, total))
