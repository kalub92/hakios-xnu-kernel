#!/bin/bash
# Build Mesa's PowerVR Vulkan driver (src/imagination, "pvr") for the iOS userland: the user-space
# half of the GPU driver of the Rabbit r1's PowerVR GE8320 (BVNC 22.87.104.18). The kernel half is
# the service RedwoodPVR (xnu/files/bsd/dev/redwood_pvr.c); the two meet in tools/mesa/redwood-drm/
# redwood_pvr_drm.c, which stands in for libdrm (the contract is at the top of that file).
#
# Beside tools/mk-mesa.sh and apart from it: another Mesa (26.3.0 -- pvr on the 8XE cores and Zink
# on pvr are of 2026; mk-mesa.sh's 25.0.7 is the last with OSMesa), other patches (tools/mesa/pvr/,
# where mk-mesa.sh's *.patch does not find them) and its own directory, xnu-deps/build/pvr-mesa.
#
#   ZINK=1      also OpenGL ES over the Vulkan driver: Zink as the gallium driver under EGL's
#               surfaceless platform (libEGL, libGLESv2 and libgallium)
#
# What comes out, in xnu-deps/build/pvr-mesa/out:
#   libvulkan_powervr_mesa.dylib    the Vulkan driver (install name /redwood/lib/...). There is no
#                                   Vulkan loader here: it also exports vkGetInstanceProcAddr and
#                                   vkGetDeviceProcAddr, and is what Zink opens.
#   with ZINK=1: libEGL.1.dylib libGLESv2.2.dylib libgallium-26.3.0.dylib
#                and exported-gl.txt, gl.xml: the gl* functions and their prototypes, for userland/mkeagl.py
#
# The driver's built-in shaders are OpenCL C, compiled while building by programs of Mesa's that
# need LLVM with clang and the SPIR-V translator. So there are three builds: the translator and
# SPIRV-Tools for this machine (into pvr-mesa/host), Mesa's compilers for this machine (mesa_clc,
# vtn_bindgen2, pco_clc, pdsc_precomp, into pvr-mesa/host/bin), and Mesa for the phone.
# Needs on this machine: ninja, cmake, git, flex, python3 with venv, and LLVM with clang
# and its libraries (LLVM=/opt/homebrew/opt/llvm; the translator's branch below is that LLVM's).
set -e
VERSION=26.3.0
SHA256=91d729aa68bb89e4ec857841300bbe922bcb3e52b1f97566ca83930d3b28343a
SPIRV_HEADERS=cb42dec3830d3ac67fa449ecdc0c0f73d5e74498   # what the translator's branch asks for
SPIRV_TOOLS=v2026.4
LLVM=${LLVM:-/opt/homebrew/opt/llvm}
TRANSLATOR=llvm_release_$("$LLVM/bin/llvm-config" --version | cut -d. -f1)0
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
source "$ROOT/xnu-env.sh"
B=$ROOT/xnu-deps/build/pvr-mesa
SDK=$THEOS/sdks/iPhoneOS16.5.sdk
CC=$ROOT/xnu-deps/host/bin/xnu-clang
H=$B/host
S=$B/src/mesa-$VERSION
mkdir -p "$B/dl" "$B/src" "$B/out"
cd "$B"

# the sources: the archive's with tools/mesa/pvr/*.patch on top (taken anew when the patches change)
PATCHES=$(cat "$ROOT"/tools/mesa/pvr/*.patch | shasum -a 256 | cut -d' ' -f1)
if [ ! -d "$S" ] || [ "$(cat "$S/.redwood-patches" 2>/dev/null)" != "$PATCHES" ]; then
	[ -f dl/mesa-$VERSION.tar.xz ] || curl -sL --fail -o dl/mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	echo "$SHA256  dl/mesa-$VERSION.tar.xz" | shasum -a 256 -c --quiet
	rm -rf "$S" build-ios build-ios-zink
	tar xJf dl/mesa-$VERSION.tar.xz -C src
	for p in "$ROOT"/tools/mesa/pvr/*.patch; do patch -s --no-backup-if-mismatch -d "$S" -p1 < "$p"; done
	echo "$PATCHES" > "$S/.redwood-patches"
fi
# ours, kept in the repository: always the current ones
rm -rf "$S/src/redwood-drm"
cp -Rp "$ROOT/tools/mesa/redwood-drm" "$S/src/redwood-drm"

if [ ! -x venv/bin/meson ]; then
	python3 -m venv venv
	venv/bin/pip -q install meson mako pyyaml packaging
fi
export PATH=$B/venv/bin:$H/bin:$PATH

# 1. for this machine: SPIRV-Tools and the SPIRV-LLVM-Translator, as static libraries
if [ ! -f "$H/lib/pkgconfig/LLVMSPIRVLib.pc" ] || [ ! -f "$H/lib/pkgconfig/SPIRV-Tools.pc" ]; then
	(
	cd dl
	[ -d SPIRV-Headers ] || git clone -q https://github.com/KhronosGroup/SPIRV-Headers
	git -C SPIRV-Headers checkout -q $SPIRV_HEADERS
	[ -d SPIRV-Tools ] || git clone -q --depth 1 -b $SPIRV_TOOLS https://github.com/KhronosGroup/SPIRV-Tools
	[ -d SPIRV-LLVM-Translator ] || git clone -q --depth 1 -b $TRANSLATOR https://github.com/KhronosGroup/SPIRV-LLVM-Translator
	cmake -S SPIRV-Headers -B b-hdr -G Ninja -DCMAKE_INSTALL_PREFIX="$H" > b-hdr.log
	ninja -C b-hdr install >> b-hdr.log
	cmake -S SPIRV-Tools -B b-tools -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$H" \
		-DSPIRV-Headers_SOURCE_DIR="$PWD/SPIRV-Headers" -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON \
		-DSPIRV_WERROR=OFF -DBUILD_SHARED_LIBS=OFF > b-tools.log 2>&1
	ninja -C b-tools install >> b-tools.log 2>&1
	cmake -S SPIRV-LLVM-Translator -B b-tr -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$H" \
		-DLLVM_DIR="$LLVM/lib/cmake/llvm" -DLLVM_EXTERNAL_SPIRV_HEADERS_SOURCE_DIR="$PWD/SPIRV-Headers" \
		-DLLVM_SPIRV_BUILD_EXTERNAL=YES -DBUILD_SHARED_LIBS=OFF -DLLVM_INCLUDE_TESTS=OFF \
		-DCMAKE_PREFIX_PATH="$H" > b-tr.log 2>&1
	ninja -C b-tr install >> b-tr.log 2>&1
	rm -rf b-hdr b-tools b-tr           # some 600 MB of objects
	)
fi

# what none of the three builds of Mesa wants
COMMON="-Dglx=disabled -Dgbm=disabled -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled
	-Dshader-cache=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs=
	-Dgallium-va=disabled -Dlmsensors=disabled -Dbuild-tests=false -Dmicrosoft-clc=disabled"

# 2. for this machine: Mesa's compilers of the driver's built-in shaders
if [ ! -x "$H/bin/pco_clc" ] || [ ! -x "$H/bin/pdsc_precomp" ] || [ ! -x "$H/bin/mesa_clc" ] || [ ! -x "$H/bin/vtn_bindgen2" ]; then
	cat > native.ini <<INI
[binaries]
llvm-config = '$LLVM/bin/llvm-config'
INI
	rm -rf build-host
	PKG_CONFIG_PATH=$H/lib/pkgconfig meson setup build-host "$S" --native-file native.ini --wrap-mode=nofallback \
		--prefix="$H" --libdir=lib --buildtype=release \
		-Dplatforms= -Dgallium-drivers= -Dvulkan-drivers= -Degl=disabled -Dopengl=false -Dgles1=disabled \
		-Dgles2=disabled -Dtools=imagination -Dllvm=enabled -Dshared-llvm=enabled -Dmesa-clc=enabled \
		-Dinstall-mesa-clc=true -Dprecomp-compiler=enabled -Dinstall-precomp-compiler=true $COMMON > meson-host.log
	ninja -C build-host install > ninja-host.log
	rm -rf build-host
fi

# 3. for the phone. The compiler is the kernel's wrapper, as in tools/mk-mesa.sh.
AR=llvm-ar RANLIB=llvm-ranlib STRIP=llvm-strip
command -v llvm-ar >/dev/null || { AR=ar; RANLIB=ranlib; STRIP=strip; }
cat > cross.ini <<INI
[binaries]
c = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK']
cpp = ['$CC++', '-arch', 'arm64', '-isysroot', '$SDK', '-fuse-cxa-atexit']
objc = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK']
ar = '$AR'
ranlib = '$RANLIB'
strip = '$STRIP'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI
# the stand-in for libdrm, an archive that Mesa's libraries are linked with (the top meson.build); a
# library that has an older one in it is linked again
(cd "$S/src/redwood-drm" && $CC -arch arm64 -isysroot "$SDK" -Os -fvisibility=hidden -I../../include -I. \
	-c redwood_pvr_drm.c -o redwood_pvr_drm.o && rm -f libredwood_drm.a && $AR rcs libredwood_drm.a redwood_pvr_drm.o)
find build-ios build-ios-zink -name '*.dylib' ! -newer "$S/src/redwood-drm/redwood_pvr_drm.c" -delete 2>/dev/null || true
CROSS="--cross-file cross.ini --wrap-mode=nofallback --prefix=/redwood --libdir=lib --buildtype=release
	-Doptimization=s -Db_ndebug=true -Dplatforms= -Dllvm=disabled
	-Dmesa-clc=system -Dprecomp-compiler=system -Dspirv-tools=disabled -Dtools= $COMMON"

[ -f build-ios/build.ninja ] || meson setup build-ios "$S" $CROSS -Dvulkan-drivers=imagination -Dgallium-drivers= -Degl=disabled \
	-Dopengl=false -Dgles1=disabled -Dgles2=disabled > meson-ios.log
ninja -C build-ios > ninja-ios.log
$STRIP -x -o out/libvulkan_powervr_mesa.dylib build-ios/src/imagination/vulkan/libvulkan_powervr_mesa.dylib
install_name_tool -id /redwood/lib/libvulkan_powervr_mesa.dylib out/libvulkan_powervr_mesa.dylib
# (userland/build.sh comes here again when this no longer matches)
cat "$ROOT"/tools/mk-mesa-pvr.sh "$ROOT"/tools/mesa/pvr/*.patch "$ROOT"/tools/mesa/redwood-drm/* | shasum -a 256 > out/built-from
echo "mk-mesa-pvr.sh: Mesa $VERSION: $B/out/libvulkan_powervr_mesa.dylib, $(wc -c < out/libvulkan_powervr_mesa.dylib | tr -d " ") bytes"

if [ "${ZINK:-}" = 1 ]; then
	# the compiler of the shading language wants bison 3; a Mac's own is 2.3
	if ! bison --version | head -1 | grep -q ' [3-9]\.'; then
		[ -f dl/bison-3.8.2.tar.xz ] || curl -sL --fail -o dl/bison-3.8.2.tar.xz https://ftp.gnu.org/gnu/bison/bison-3.8.2.tar.xz
		echo "9bba0214ccf7f1079c5d59210045227bcf619519840ebfa80cd3849cff5a5bf2  dl/bison-3.8.2.tar.xz" | shasum -a 256 -c --quiet
		tar xJf dl/bison-3.8.2.tar.xz -C dl
		(cd dl/bison-3.8.2 && ./configure --prefix="$H" --disable-nls > ../bison.log 2>&1 && make -j8 install >> ../bison.log 2>&1)
		rm -rf dl/bison-3.8.2
	fi
	# (without the Vulkan driver, which Zink opens as a library of its own)
	[ -f build-ios-zink/build.ninja ] || meson setup build-ios-zink "$S" $CROSS -Dvulkan-drivers= -Dgallium-drivers=zink -Degl=enabled \
		-Dopengl=true -Dgles1=disabled -Dgles2=enabled > meson-ios-zink.log
	ninja -C build-ios-zink > ninja-ios-zink.log
	for l in src/egl/libEGL.1.dylib src/mesa/glapi/es2api/libGLESv2.2.dylib src/gallium/targets/dri/libgallium-$VERSION.dylib; do
		$STRIP -x -o out/$(basename $l) build-ios-zink/$l
		install_name_tool -id /redwood/lib/$(basename $l) \
			-change @rpath/libgallium-$VERSION.dylib /redwood/lib/libgallium-$VERSION.dylib out/$(basename $l)
	done
	# for userland/mkeagl.py: the gl* functions the library exports, and their prototypes (the Khronos registry)
	nm -gU out/libGLESv2.2.dylib | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > out/exported-gl.txt
	cp "$S/src/mesa/glapi/glapi/registry/gl.xml" out/gl.xml
	cp out/built-from out/built-from-zink
	echo "mk-mesa-pvr.sh: Zink: $(cd out && ls -l libEGL.1.dylib libGLESv2.2.dylib libgallium-$VERSION.dylib | awk '{ printf "%s %s bytes; ", $9, $5 }')"
fi
