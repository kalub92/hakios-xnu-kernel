#!/bin/bash
# Build the small tools that are run on the phone under Android (adb's root shell): static arm64
# Linux binaries with no C library, so nothing beyond clang and lld is needed.
set -e
cd "$(dirname "$0")"
for t in f2fs-resize r1camdump; do
	clang --target=aarch64-linux-gnu -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static \
		-fuse-ld=lld -O1 -Wall -Wl,-e,_start -o $t $t.c
	ls -la $t
done
