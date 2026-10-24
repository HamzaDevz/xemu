#!/usr/bin/env bash
# Builds the hakuX core (android/hakux/core, GPLv3) into libhakuxcore.so for
# the app's second, QEMU-free core (HakuxCoreActivity, process :hakux).
#
# The core is its own CMake project, not part of src/main/cpp: it brings its
# own glslang, volk and FEX, whose targets would clash with libxemu's. Gradle
# runs this before it merges native libraries (task buildHakuxCore) and packs
# the result from build/hakux-jniLibs.
#
#   android/hakux/build-core.sh [Release|Debug]
#
# Environment:
#   ANDROID_NDK_HOME  the NDK (default: the app's ndkVersion under ~/Android/Sdk)
#   X1BOX_FEX_DIR     the FEX fork with the system-mode API (branch x1box-haku)
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
android=$(cd "$here/.." && pwd)
build_type=${1:-Release}
ndk=${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/30.0.15729638}
fex=${X1BOX_FEX_DIR:-$(cd "$android/../.." && pwd)/FEX}
deps=$here/.deps
build=$here/.build/$build_type
out=$android/app/build/hakux-jniLibs/arm64-v8a

if [ ! -f "$fex/FEXCore/include/FEXCore/Core/Context.h" ] ||
   ! grep -q EnableSystemOps "$fex/FEXCore/include/FEXCore/Core/Context.h"; then
  echo "build-core.sh: no FEX fork with the system-mode API at $fex (set X1BOX_FEX_DIR)" >&2
  exit 1
fi

# Third-party sources at the revisions hakuX v1.0.0 pins (android/README.md
# there); SDL is the app's own version, so libhakuxcore.so runs against the
# libSDL3.so libxemu ships with and the one Java glue layer.
fetch() { # <dir> <url> <rev>
  local dir=$deps/$1
  if [ ! -d "$dir/.git" ]; then
    git clone -q "$2" "$dir"
  fi
  if [ "$(git -C "$dir" rev-parse HEAD)" != "$(git -C "$dir" rev-parse "$3^{commit}" 2>/dev/null)" ]; then
    git -C "$dir" fetch -q --tags origin || true
    git -C "$dir" checkout -q "$3"
  fi
  git -C "$dir" submodule update -q --init --recursive
}
mkdir -p "$deps/root/subprojects"
fetch volk https://github.com/zeux/volk.git 0b17a763ba5643e32da1b2152f8140461b3b7345
fetch glslang https://github.com/KhronosGroup/glslang.git 16.1.0
fetch spirv_reflect https://github.com/KhronosGroup/SPIRV-Reflect.git vulkan-sdk-1.4.328.0
fetch vma https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git v3.3.0
fetch SDL3 https://github.com/libsdl-org/SDL.git release-3.4.14
fetch adrenotools https://github.com/bylaws/libadrenotools.git 8fae8ce
fetch root/subprojects/nv2a_vsh_cpu https://github.com/xemu-project/nv2a_vsh_cpu.git 1115255708c10c4841b65dcd2223262e7a316598

if [ ! -f "$build/build.ninja" ]; then
  cmake -S "$here/core/native" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
    -DCMAKE_BUILD_TYPE="$build_type" \
    -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
    -DHAKUX_FEX_DIR="$fex" -DNC_DEPS="$deps"
fi
ninja -C "$build" hakuxcore

mkdir -p "$out"
strip=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
# Only the core: libSDL3.so and adrenotools' hook libraries come from libxemu's build.
"$strip" -o "$out/libhakuxcore.so.tmp" "$build/libhakuxcore.so"
mv "$out/libhakuxcore.so.tmp" "$out/libhakuxcore.so"
ls -la "$out/libhakuxcore.so"
