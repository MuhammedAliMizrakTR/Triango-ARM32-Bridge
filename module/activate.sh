#!/system/bin/sh
# SPDX-License-Identifier: MIT
# Triango ARM32 Bridge — soft-reboot activator (v1.0).
#
#   su -c /data/adb/modules/triango_arm32_bridge/activate.sh [--no-restart]
#
# Activates (or re-activates) the Triango stack WITHOUT a full reboot:
#   1. props (abilist + native.bridge) via resetprop
#   2. linkerconfig permission for /data/adb/triango/lib
#   3. zygote restart ("soft reboot") unless --no-restart
#
# Called from the WebUI's "Aktive Et" button and manually from a root shell.
MODDIR=${0%/*}
TRI=/data/adb/triango

log() {
	echo "triango: $*" > /dev/kmsg 2>/dev/null || true
	echo "triango: $*"
}

RESETPROP=""
for c in resetprop /data/adb/ksu/bin/resetprop /data/adb/magisk/resetprop \
	/system/bin/resetprop; do
	if command -v "$c" >/dev/null 2>&1; then
		RESETPROP="$c"
		break
	fi
done

if [ -z "$RESETPROP" ]; then
	log "ERROR: resetprop not found; cannot set props at runtime"
	exit 1
fi

# 1. Stage the bridge library (latest build wins).
mkdir -p "$TRI/lib" "$TRI/log" "$TRI/config"
if [ ! -f "$TRI/config/disable_nativebridge" ]; then
	for f in "$MODDIR/system/lib64/libtriango_native_bridge.so" \
		"$MODDIR/libtriango_native_bridge.so"; do
		if [ -f "$f" ]; then
			cp -f "$f" "$TRI/lib/libtriango_native_bridge.so" 2>/dev/null
			chmod 644 "$TRI/lib/libtriango_native_bridge.so" 2>/dev/null
			break
		fi
	done

	# 2. Linker namespace permission (idempotent).
	LC=/linkerconfig/ld.config.txt
	if [ -f "$LC" ] && ! grep -q "data/adb/triango/lib" "$LC" 2>/dev/null; then
		sed -i 's#namespace.default.search.paths = #namespace.default.search.paths = /data/adb/triango/lib: #' "$LC" 2>/dev/null
		sed -i 's#namespace.default.permitted.paths = #namespace.default.permitted.paths = /data/adb/triango/lib: #' "$LC" 2>/dev/null
		log "linkerconfig patched"
	fi

	# 3. Native bridge prop (absolute path preferred).
	if [ -f "$TRI/lib/libtriango_native_bridge.so" ]; then
		"$RESETPROP" ro.dalvik.vm.native.bridge \
			"$TRI/lib/libtriango_native_bridge.so" 2>/dev/null
	else
		"$RESETPROP" ro.dalvik.vm.native.bridge \
			"libtriango_native_bridge.so" 2>/dev/null
		log "WARN: bridge .so not staged (build with tools/build-nativebridge.sh)"
	fi
fi

# 4. ABI props.
if [ ! -f "$TRI/config/disable_abilist32" ]; then
	"$RESETPROP" ro.product.cpu.abilist "arm64-v8a,armeabi-v7a,armeabi" 2>/dev/null
	"$RESETPROP" ro.product.cpu.abilist32 "armeabi-v7a,armeabi" 2>/dev/null
	"$RESETPROP" ro.product.cpu.abilist64 "arm64-v8a" 2>/dev/null
	"$RESETPROP" ro.vendor.product.cpu.abilist "arm64-v8a,armeabi-v7a,armeabi" 2>/dev/null
	"$RESETPROP" ro.vendor.product.cpu.abilist32 "armeabi-v7a,armeabi" 2>/dev/null
fi

if [ "$1" = "--no-restart" ]; then
	log "props applied (zygote restart skipped)"
	exit 0
fi

# 5. Soft reboot: restart the Android framework so zygote re-reads props.
log "soft reboot: restarting zygote"
setprop ctl.restart zygote 2>/dev/null
setprop ctl.restart zygote_secondary 2>/dev/null
# Fallback for older init:
stop 2>/dev/null
start 2>/dev/null
log "soft reboot requested"
