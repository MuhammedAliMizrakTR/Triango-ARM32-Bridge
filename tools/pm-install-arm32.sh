#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Install an arm32 (armeabi-v7a) APK on a 64-bit-only device by overriding
# the PackageManager ABI filter for a single install.
#
#   ./tools/pm-install-arm32.sh oyun.apk [more.apk ...]
#
# This is the per-install workaround for INSTALL_FAILED_NO_MATCHING_ABIS.
# For the system-wide fix (ro.product.cpu.abilist32 via the Triango module)
# see docs/abi-fix.md.
set -euo pipefail

if [[ $# -lt 1 ]]; then
	echo "usage: $0 <arm32.apk> [more.apk ...]" >&2
	exit 2
fi

for apk in "$@"; do
	if [[ ! -f "$apk" ]]; then
		echo "error: no such file: $apk" >&2
		exit 1
	fi
	echo "== installing $apk with --abi armeabi-v7a =="
	# `adb install --abi` maps to `pm install --abi` on-device; it forces
	# the ABI selection past the SUPPORTED_ABIS filter.
	if ! adb install --abi armeabi-v7a "$apk"; then
		echo "adb install --abi failed; retrying via pm on-device..." >&2
		adb push "$apk" /data/local/tmp/triango-install.apk
		adb shell pm install --abi armeabi-v7a /data/local/tmp/triango-install.apk
		adb shell rm -f /data/local/tmp/triango-install.apk
	fi
done
echo "done. NOTE: installation works, app-level execution needs roadmap v0.3 (NativeBridge/zygote32)."
