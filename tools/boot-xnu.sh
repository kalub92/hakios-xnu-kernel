#!/bin/bash
# Reboot the X6826B into Darwin/XNU once: mtkloader starts Android unless it finds this request
# (kept across the reboot by ramoops' pmsg zone); the boot after XNU is Android again.
set -e
S="${HAKIOS_SERIAL:?set HAKIOS_SERIAL to your phone adb serial}"
adb -s $S root >/dev/null 2>&1; sleep 2; adb -s $S wait-for-device  # r1: userdebug adb root instead of su
# r1: a normal reboot keeps no DRAM, so the request goes through the flash (mtkloader reads dtbo_b's first sector)
adb -s $S shell '(printf DARWIN-REDWOOD:BOOT-XNU; head -c 489 /dev/zero) | dd of=/dev/block/by-name/dtbo_b bs=512 count=1 conv=fsync 2>/dev/null; sync' 
adb -s $S reboot
