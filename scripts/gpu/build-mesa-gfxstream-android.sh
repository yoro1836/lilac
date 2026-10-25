#!/bin/bash
# Builds the guest's Vulkan HAL, Mesa's gfxstream-vk for Android with the
# kumquat transport: vulkan.gfxstream.so, rendering through the app's
# vine_kumquat server. NDK + Mesa's android-stub; the kumquat FFI is a
# cargo static library (Mesa patches 01/02, from TAWC).
#
# Usage: scripts/gpu/build-mesa-gfxstream-android.sh [--clean]
# Output: build/mesa-android-aarch64/vulkan.gfxstream.so
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$REPO_DIR/scripts/lib/deps.sh"

CLEAN=0
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        *) echo "ERROR: unknown arg: $arg" >&2; exit 1 ;;
    esac
done

ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
NDK_DIR="$(ls -d "$ANDROID_HOME"/ndk/* 2>/dev/null | sort -V | tail -1)"
[ -d "$NDK_DIR" ] || { echo "ERROR: no NDK at $ANDROID_HOME/ndk/*" >&2; exit 1; }
NDK_BIN="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin"
# The guest is Android 13
API=33
TRIPLE=aarch64-linux-android
RUST_TARGET=aarch64-linux-android
CC="$NDK_BIN/${TRIPLE}${API}-clang"
CXX="$NDK_BIN/${TRIPLE}${API}-clang++"

for tool in meson ninja pkg-config cargo; do
    command -v "$tool" >/dev/null || { echo "ERROR: '$tool' not on PATH" >&2; exit 1; }
done

MESA_DIR="$(dep_dir mesa)"
dep_apply_patches mesa "$REPO_DIR/deps/mesa-patches/mesa"
GFXSTREAM_DIR="$(dep_dir gfxstream)"
dep_ensure gfxstream

OUT_DIR="$REPO_DIR/build/mesa-android-aarch64"
BUILD_DIR="$OUT_DIR/build"
PC_DIR="$OUT_DIR/pkgconfig"
CARGO_DIR="$OUT_DIR/cargo"
SHIM_DIR="$OUT_DIR/include-shims"
[ "$CLEAN" = 1 ] && rm -rf "$OUT_DIR"
mkdir -p "$PC_DIR" "$CARGO_DIR/.cargo" "$SHIM_DIR"

# ── kumquat FFI ──
cat >"$CARGO_DIR/.cargo/config.toml" <<EOF
[target.${RUST_TARGET}]
linker = "$CC"
EOF
( cd "$MESA_DIR" && CARGO_TARGET_DIR="$CARGO_DIR/target" CARGO_HOME="$CARGO_DIR" \
    cargo build -p virtgpu_kumquat_ffi --target="$RUST_TARGET" --release )
FFI_A="$CARGO_DIR/target/$RUST_TARGET/release/libvirtgpu_kumquat_ffi.a"
FFI_INC="$MESA_DIR/src/virtio/virtgpu_kumquat_ffi/include"
cat >"$PC_DIR/virtgpu_kumquat_ffi.pc" <<EOF
Name: virtgpu_kumquat_ffi
Description: kumquat FFI (cargo)
Version: 0.1.76
Cflags: -I$FFI_INC
Libs: $FFI_A -ldl -lm
EOF

# ── headers the NDK and android-stub lack ──
# gfxstream's goldfish gralloc handle
cp -f "$GFXSTREAM_DIR/guest/gralloc_cb/include/gralloc_cb_bp.h" "$SHIM_DIR/"
mkdir -p "$SHIM_DIR/android-base"
cat >"$SHIM_DIR/android-base/properties.h" <<'EOF'
// libbase's GetProperty over the NDK's system properties
#pragma once
#include <string>
#include <sys/system_properties.h>
namespace android::base {
inline std::string GetProperty(const std::string& key, const std::string& default_value) {
    char v[PROP_VALUE_MAX] = {};
    return __system_property_get(key.c_str(), v) > 0 ? std::string(v) : default_value;
}
}  // namespace android::base
EOF

cat >"$OUT_DIR/cross.txt" <<EOF
[binaries]
c = ['$CC', '-I$SHIM_DIR']
cpp = ['$CXX', '-I$SHIM_DIR']
ar = '$NDK_BIN/llvm-ar'
strip = '$NDK_BIN/llvm-strip'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = '$PC_DIR'

[built-in options]
# bionic marks tss_set's value nonnull
c_args = ['-Wno-error=nonnull', '-DANDROID=1']
cpp_args = ['-Wno-error=nonnull', '-DANDROID=1']
# The guest has no libc++_shared
cpp_link_args = ['-static-libstdc++']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF

if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    PKG_CONFIG_LIBDIR="$PC_DIR" meson setup "$BUILD_DIR" "$MESA_DIR" \
        --cross-file "$OUT_DIR/cross.txt" \
        -Dplatforms=android -Dandroid-stub=true -Dandroid-libbacktrace=disabled \
        -Dplatform-sdk-version=$API \
        -Dvulkan-drivers=gfxstream -Dgallium-drivers= \
        -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Degl=disabled -Dglx=disabled \
        -Dvideo-codecs= -Dllvm=disabled -Dxmlconfig=disabled -Dvalgrind=disabled \
        -Dlibunwind=disabled -Dlmsensors=disabled -Dzstd=disabled \
        -Dvirtgpu_kumquat=true -Dvirtgpu_kumquat_external_ffi=true \
        -Dallow-fallback-for=libdrm -Dlibdrm:intel=disabled -Dlibdrm:radeon=disabled -Dlibdrm:amdgpu=disabled -Dlibdrm:nouveau=disabled -Dlibdrm:vmwgfx=disabled -Dlibdrm:tests=false \
        --buildtype release -Ddefault_library=shared
fi
ninja -C "$BUILD_DIR" src/gfxstream/guest/vulkan/libvulkan_gfxstream.so
cp "$BUILD_DIR/src/gfxstream/guest/vulkan/libvulkan_gfxstream.so" "$OUT_DIR/vulkan.gfxstream.so"
"$NDK_BIN/llvm-strip" "$OUT_DIR/vulkan.gfxstream.so"
ls -la "$OUT_DIR/vulkan.gfxstream.so"
