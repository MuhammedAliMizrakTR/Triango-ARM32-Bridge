#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build a static AArch64 qemu-arm (linux-user) engine for the Triango bridge.
# Run this on a Linux host (WSL works), NOT on the phone.
#
#   ./tools/build-qemu-engine.sh                 # clones qemu from GitHub
#   QEMU_SRC=/path/to/qemu ./tools/build-qemu-engine.sh
#
# Output: build/qemu-arm  ->  copy to the device as
#   /data/adb/triango/engines/qemu-arm
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QEMU_SRC="${QEMU_SRC:-$ROOT/build/qemu-src}"
QEMU_TAG="${QEMU_TAG:-v9.2.0}"

if [[ ! -d "$QEMU_SRC/.git" ]]; then
	echo "cloning qemu ($QEMU_TAG) into $QEMU_SRC ..."
	git clone --depth=1 --branch "$QEMU_TAG" \
		https://github.com/qemu/qemu "$QEMU_SRC"
fi

cd "$QEMU_SRC"
./configure \
	--target-list=arm-linux-user \
	--static \
	--disable-system \
	--disable-tools \
	--disable-docs \
	--disable-guest-agent
make -j"$(nproc)" qemu-arm

mkdir -p "$ROOT/build"
cp "$QEMU_SRC/build/qemu-arm" "$ROOT/build/qemu-arm"
strip "$ROOT/build/qemu-arm" 2>/dev/null || true
echo
echo "built: $ROOT/build/qemu-arm"
echo "deploy: adb push build/qemu-arm /sdcard/ && adb shell su -c 'mkdir -p /data/adb/triango/engines && cp /sdcard/qemu-arm /data/adb/triango/engines/ && chmod 755 /data/adb/triango/engines/qemu-arm'"
