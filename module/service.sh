#!/system/bin/sh
# SPDX-License-Identifier: MIT
# Triango ARM32 Bridge — late_start service (v1.0).
#
# LateLoad-compatible: runs after boot_completed (BakaSU ksud late-load may
# schedule this even later).  Everything is idempotent — it is safe to run
# the module's activate script repeatedly, and we do exactly that here to
# cover the case where post-fs-data ran before the .so was staged.
MODDIR=${0%/*}
TRI=/data/adb/triango

# Wait for the boot to settle (max ~2 min).
i=0
while [ "$(getprop sys.boot_completed)" != "1" ] && [ "$i" -lt 240 ]; do
	sleep 1
	i=$((i + 1))
done

# Re-apply props + staging without bouncing zygote (it is up by now):
# props only matter for the NEXT zygote start; the WebUI's Activate button
# performs the actual soft reboot on demand.
sh "$MODDIR/activate.sh" --no-restart

# Re-assert the ABI advertisement (idempotent): covers LateLoad ordering
# where post-fs-data ran late or a vendor service reset the props.
if [ ! -f "$TRI/config/disable_abilist32" ]; then
	RESETPROP=""
	for c in resetprop /data/adb/ksu/bin/resetprop /data/adb/magisk/resetprop \
		/system/bin/resetprop; do
		if command -v "$c" >/dev/null 2>&1; then
			RESETPROP="$c"
			break
		fi
	done
	if [ -n "$RESETPROP" ]; then
		"$RESETPROP" ro.product.cpu.abilist "arm64-v8a,armeabi-v7a,armeabi" 2>/dev/null
		"$RESETPROP" ro.product.cpu.abilist32 "armeabi-v7a,armeabi" 2>/dev/null
		"$RESETPROP" ro.product.cpu.abilist64 "arm64-v8a" 2>/dev/null
		"$RESETPROP" ro.vendor.product.cpu.abilist "arm64-v8a,armeabi-v7a,armeabi" 2>/dev/null
		"$RESETPROP" ro.vendor.product.cpu.abilist32 "armeabi-v7a,armeabi" 2>/dev/null
		echo "triango: abilist32 re-asserted" > /dev/kmsg 2>/dev/null
	fi
fi

# Start the GPU thunk daemon if its binary was staged.
GPUD="$TRI/triango-gpud"
if [ -x "$GPUD" ] && [ ! -f "$TRI/config/disable_gpud" ]; then
	if ! pidof triango-gpud >/dev/null 2>&1; then
		"$GPUD" >> "$TRI/log/gpud.log" 2>&1 &
		echo "triango: gpud started" > /dev/kmsg 2>/dev/null
	fi
fi

echo "triango: service done" > /dev/kmsg 2>/dev/null
