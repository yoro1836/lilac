#!/bin/bash
# Builds every piece of GPU acceleration (graphics=gfxstream) and stages
# it in app/src/main/jniLibs/arm64-v8a:
#   libgfxstream_backend.so, libc++_shared.so  host renderer (TAWC's build)
#   libvine_kumquat.so                          the GPU and gralloc server
#   libvine_vk_gfxstream.so                     the guest's Vulkan HAL,
#                                               copied into its /vendor
# Usage: scripts/gpu/build-gpu.sh [--clean]
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$REPO_DIR/scripts/lib/deps.sh"

ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
NDK_DIR="$(ls -d "$ANDROID_HOME"/ndk/* 2>/dev/null | sort -V | tail -1)"
[ -d "$NDK_DIR" ] || { echo "ERROR: no NDK at $ANDROID_HOME/ndk/*" >&2; exit 1; }
NDK_BIN="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin"
JNI="$REPO_DIR/app/src/main/jniLibs/arm64-v8a"
mkdir -p "$JNI"

ANDROID_HOME="$ANDROID_HOME" "$SCRIPT_DIR/build-gfxstream-backend.sh" --abi=aarch64 "$@"
ANDROID_HOME="$ANDROID_HOME" "$SCRIPT_DIR/build-mesa-gfxstream-android.sh" "$@"
cp "$REPO_DIR/build/mesa-android-aarch64/vulkan.gfxstream.so" "$JNI/libvine_vk_gfxstream.so"

dep_apply_patches rutabaga_gfx "$REPO_DIR/deps/rutabaga-patches/rutabaga_gfx"
# GFXSTREAM_PATH skips pkg-config, which is what would turn on
# gfxstream_unstable; the backend has those symbols (fence export: the
# guest asks for sync fds), so set the cfg by hand.
CC="$NDK_BIN/aarch64-linux-android28-clang"
( cd "$REPO_DIR/gpu/kumquat" &&
  CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$CC" \
  CC_aarch64_linux_android="$CC" \
  CXX_aarch64_linux_android="$NDK_BIN/aarch64-linux-android28-clang++" \
  AR_aarch64_linux_android="$NDK_BIN/llvm-ar" \
  GFXSTREAM_PATH="$REPO_DIR/build/gfxstream-android-aarch64" \
  CARGO_TARGET_DIR="$REPO_DIR/build/kumquat" \
  RUSTFLAGS='-C link-arg=-Wl,-rpath,$ORIGIN -C link-arg=-Wl,-z,max-page-size=16384 --cfg gfxstream_unstable' \
  cargo build --release --target aarch64-linux-android )
cp "$REPO_DIR/build/kumquat/aarch64-linux-android/release/vine_kumquat" "$JNI/libvine_kumquat.so"
"$NDK_BIN/llvm-strip" "$JNI/libvine_kumquat.so"
ls -la "$JNI"/libgfxstream_backend.so "$JNI"/libvine_kumquat.so "$JNI"/libvine_vk_gfxstream.so
