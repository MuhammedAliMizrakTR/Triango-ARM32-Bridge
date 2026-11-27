#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build libtriango_native_bridge.so for Android (aarch64, bionic).
#
#   ./tools/build-nativebridge.sh /path/to/android-ndk [api]
#
# Output: nativebridge/prebuilt/libtriango_native_bridge.so
# (tools/package-module.sh picks it up from there.)
#
# Host smoke builds (no NDK) are available via: make -C nativebridge lib
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NDK="${1:-${ANDROID_NDK_HOME:-${ANDROID_NDK:-}}}"
API="${2:-29}"

if [[ -z "$NDK" || ! -d "$NDK" ]]; then
	echo "usage: $0 /path/to/android-ndk [api]" >&2
	echo "  (or set ANDROID_NDK_HOME)" >&2
	exit 2
fi

PREBUILT="$NDK/toolchains/llvm/prebuilt"
HOST_TAG="$(ls "$PREBUILT" | head -1)"
CC="$PREBUILT/$HOST_TAG/bin/clang"
TARGET=aarch64-linux-android$API-clang

if [[ ! -x "$CC" ]]; then
	echo "error: NDK clang not found under $PREBUILT" >&2
	exit 1
fi

OUT_DIR="$ROOT/nativebridge/prebuilt"
mkdir -p "$OUT_DIR"

"$CC" --target="$TARGET" -std=gnu11 -O2 -Wall -Wextra -fPIC -shared \
	-I"$ROOT/nativebridge/include" \
	-o "$OUT_DIR/libtriango_native_bridge.so" \
	"$ROOT/nativebridge/lib/nb_triango.c" \
	"$ROOT/nativebridge/lib/nb_engine.c" \
	"$ROOT/nativebridge/lib/nb_elf.c" \
	-ldl -llog

echo "built: $OUT_DIR/libtriango_native_bridge.so"
