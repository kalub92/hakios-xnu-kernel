#!/bin/bash
# r1: one test run. usage: tools/r1-run.sh NAME SECONDS "extra boot-args" [build]
# "build" as the 4th argument rebuilds the kernel and the payload first. Prints the drivers' kept lines.
cd "$(dirname "$0")/.." || exit 1
export HAKIOS_SERIAL=${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the adb serial} PATH=/opt/homebrew/opt/lld/bin:$PATH
if [ "$4" = build ]; then
	./xnu-build.sh -j10 -k > logs/xnubuild.log 2>&1 || { grep -E "error:" logs/xnubuild.log | head -20; exit 1; }
	XL_RAMDISK=userland/root.apfs ./xl_x6826b build 2>&1 | tail -1
fi
XNU_SECS=$2 XNU_ARGS="cpus=${CPUS:-4} touchscreen=3 emmc_lba=9262616 emmc_blocks=4966400 $3" XNU_LOG_OUT=logs/$1.log timeout ${R1_TIMEOUT:-1800} tools/xnu-cycle.sh 2>&1 | tail -2
grep -a "XNU returned\|watcher:\|loader fault\|panic" logs/$1.log | head -5
sed -n '/kept for the host by the kernel/,$p' logs/$1.log | cut -c1-400
