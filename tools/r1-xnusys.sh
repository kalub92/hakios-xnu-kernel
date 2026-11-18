#!/bin/bash
# r1: put the system image (userland/system.apfs) into a logical partition "xnusys_a" of super, which
# Android ignores, and print its flash extent for the loader and the kernel (emmc_lba / emmc_blocks).
# Nothing is wiped: super has ~5 GB free next to system_a and vendor_a. Needs Android up, adb root.
#   tools/r1-xnusys.sh create      fastbootd: create-logical-partition (size from system.apfs)
#   tools/r1-xnusys.sh locate      print SYSTEM_LBA / SYSTEM_BLOCKS (one extent is required)
#   tools/r1-xnusys.sh write       dd system.apfs to that extent through /dev/block/by-name/super
set -e
S="${HAKIOS_SERIAL:?set HAKIOS_SERIAL}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG=$ROOT/userland/system.apfs
SUPER_LBA=$((0x31000000 / 512))          # r1 GPT: super at 0x31000000
case "$1" in
create)
	MB=$(( ( $(stat -f%z "$IMG") + (64 << 20) ) >> 20 ))
	adb -s $S reboot fastboot
	for i in $(seq 1 60); do fastboot devices | grep -q $S && break; sleep 2; done
	fastboot -s $S delete-logical-partition xnusys_a >/dev/null 2>&1 || true
	fastboot -s $S create-logical-partition xnusys_a $((MB << 20))
	fastboot -s $S reboot ;;
locate)
	adb -s $S root >/dev/null; sleep 2; adb -s $S wait-for-device
	adb -s $S shell 'dd if=/dev/block/by-name/super bs=1048576 count=1 2>/dev/null' > "$ROOT/logs/super_head.bin"
	python3 "$ROOT/tools/r1-lpdump.py" "$ROOT/logs/super_head.bin" | awk '/^slot 0/{p=1} /^slot 1/{p=0} p' | grep -A3 "xnusys_a" | tee "$ROOT/logs/xnusys.txt"
	n=$(grep -c "extent " "$ROOT/logs/xnusys.txt"); [ "$n" = 1 ] || { echo "xnusys_a has $n extents; need exactly one"; exit 1; }
	read secs at < <(sed -n 's/.*extent 0: \([0-9]*\) sectors at super sector \([0-9]*\).*/\1 \2/p' "$ROOT/logs/xnusys.txt")
	echo "SYSTEM_LBA=$((SUPER_LBA + at)) SYSTEM_BLOCKS=$secs" | tee "$ROOT/logs/xnusys.env" ;;
write)
	. "$ROOT/logs/xnusys.env"
	adb -s $S root >/dev/null; sleep 2; adb -s $S wait-for-device
	adb -s $S push "$IMG" /data/local/tmp/system.apfs
	adb -s $S shell "dd if=/data/local/tmp/system.apfs of=/dev/block/by-name/super bs=512 seek=$((SYSTEM_LBA - SUPER_LBA)) conv=notrunc,fsync 2>&1 | tail -1; rm /data/local/tmp/system.apfs; sync" ;;
*) echo "usage: $0 create|locate|write"; exit 1 ;;
esac
