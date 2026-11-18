#!/bin/bash
# The shaders of userland/src/pvr_vk_test.c as arrays: userland/src/pvr_vk_test_spv.h, from the SPIR-V
# assembly here. Wants SPIRV-Tools' spirv-as and spirv-val (given as SPIRV_TOOLS=<directory>, or in PATH).
set -e
cd "$(dirname "$0")"
T=${SPIRV_TOOLS:+$SPIRV_TOOLS/}
out=../../../../userland/src/pvr_vk_test_spv.h
echo "/* Made by tools/mesa/pvr/shaders/mk.sh from the assembly there: the shaders of pvr_vk_test.c, SPIR-V 1.0. */" > $out
for s in vertex fragment compute; do
	${T}spirv-as --target-env vulkan1.0 -o /tmp/pvr_vk_test_$s.spv $s.spvasm
	${T}spirv-val --target-env vulkan1.0 /tmp/pvr_vk_test_$s.spv
	echo "static const uint32_t pvr_vk_test_$s[] = {" >> $out
	od -An -v -t x4 /tmp/pvr_vk_test_$s.spv | sed 's/ *\([0-9a-f]\{8\}\)/ 0x\1,/g; s/^/	/' >> $out
	echo "};" >> $out
	rm /tmp/pvr_vk_test_$s.spv
done
