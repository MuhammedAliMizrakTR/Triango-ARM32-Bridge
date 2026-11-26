#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Build triango_bridge.ko against a kernel source tree.
#
#   ./build.sh /path/to/kernel-common-android16-6.12 [aarch64-linux-gnu-]
#
# For the Samsung Galaxy S26 Ultra (m3q) use the GKI android16-6.12 tree,
# matching the device kernel 6.12.30-android16-5-...-4k.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
KERNEL_SRC="${1:-${KERNEL_SRC:-}}"
CROSS="${2:-${CROSS_COMPILE:-aarch64-linux-gnu-}}"

if [[ -z "$KERNEL_SRC" ]]; then
    echo "usage: $0 /path/to/kernel-source [CROSS_COMPILE]" >&2
    echo "hint: git clone --depth=1 -b android16-6.12 https://android.googlesource.com/kernel/common kernel-common" >&2
    exit 2
fi
if [[ ! -f "$KERNEL_SRC/Makefile" ]]; then
    echo "error: '$KERNEL_SRC' does not look like a kernel source tree" >&2
    exit 1
fi
if [[ ! -f "$KERNEL_SRC/include/generated/autoconf.h" ]]; then
    echo "error: kernel tree is not prepared (run make ARCH=arm64 defconfig && make ARCH=arm64 modules_prepare)" >&2
    exit 1
fi

make -C "$HERE" KERNEL_SRC="$KERNEL_SRC" ARCH=arm64 CROSS_COMPILE="$CROSS" modules

KO="$HERE/triango_bridge.ko"
if [[ -f "$KO" ]]; then
    echo
    echo "built: $KO"
    if command -v "${CROSS}modinfo" >/dev/null 2>&1; then
        "${CROSS}modinfo" "$KO" | grep -E '^(filename|license|description|vermagic|parm):' || true
    elif command -v modinfo >/dev/null 2>&1; then
        modinfo "$KO" | grep -E '^(filename|license|description|vermagic|parm):' || true
    fi
fi
