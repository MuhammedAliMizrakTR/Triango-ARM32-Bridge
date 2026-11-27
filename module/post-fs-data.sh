#!/system/bin/sh
# SPDX-License-Identifier: MIT
# Triango ARM32 Bridge — post-fs-data (v1.0).
#
# Runs BEFORE system_server caches Build.SUPPORTED_ABIS and before zygote
# reads ro.dalvik.vm.native.bridge.  Everything here is idempotent and
# LateLoad-safe (BakaSU ksud late-load may re-run service parts later).
MODDIR=${0%/*}
TRI=/data/adb/triango

log() {
	echo "triango: $*" > /dev/kmsg 2>/dev/null || true
}

mkdir -p "$TRI/engines" "$TRI/sysroot32" "$TRI/lib" "$TRI/log" "$TRI/config"
chmod 700 "$TRI" 2>/dev/null

# ---- 1. ABI advertising (INSTALL_FAILED_NO_MATCHING_ABIS fix) ----
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
		log "abilist32 advertised via $RESETPROP"
	else
		log "resetprop not found; relying on module system.prop"
	fi
fi

# ---- 2. NativeBridge: stage the bridge library + linkerconfig path ----
if [ ! -f "$TRI/config/disable_nativebridge" ]; then
	# Stage (or refresh) the bridge .so into /data/adb/triango/lib so the
	# soft-reboot activator (and zygote via linkerconfig) can reach it.
	for f in "$MODDIR/system/lib64/libtriango_native_bridge.so" \
		"$MODDIR/libtriango_native_bridge.so"; do
		if [ -f "$f" ]; then
			cp -f "$f" "$TRI/lib/libtriango_native_bridge.so" 2>/dev/null
			chmod 644 "$TRI/lib/libtriango_native_bridge.so" 2>/dev/null
			break
		fi
	done

	# Allow zygote's linker namespace to dlopen from /data/adb/triango/lib.
	# /linkerconfig is regenerated at boot; patching in post-fs-data lands
	# before zygote starts.  Idempotent: skip if already patched.
	LC=/linkerconfig/ld.config.txt
	if [ -f "$LC" ] && ! grep -q "data/adb/triango/lib" "$LC" 2>/dev/null; then
		sed -i 's#namespace.default.search.paths = #namespace.default.search.paths = /data/adb/triango/lib: #' "$LC" 2>/dev/null
		sed -i 's#namespace.default.permitted.paths = #namespace.default.permitted.paths = /data/adb/triango/lib: #' "$LC" 2>/dev/null
		log "linkerconfig patched for /data/adb/triango/lib"
	fi

	RESETPROP=""
	for c in resetprop /data/adb/ksu/bin/resetprop /data/adb/magisk/resetprop \
		/system/bin/resetprop; do
		if command -v "$c" >/dev/null 2>&1; then
			RESETPROP="$c"
			break
		fi
	done
	if [ -n "$RESETPROP" ]; then
		# Prefer the absolute path (works without magic mount); the
		# bare name falls back to system/lib64 after a full boot.
		if [ -f "$TRI/lib/libtriango_native_bridge.so" ]; then
			"$RESETPROP" ro.dalvik.vm.native.bridge \
				"$TRI/lib/libtriango_native_bridge.so" 2>/dev/null
		else
			"$RESETPROP" ro.dalvik.vm.native.bridge \
				"libtriango_native_bridge.so" 2>/dev/null
		fi
	fi
	log "native bridge props staged"
fi

# Stage module payloads into /data/adb/triango.
if [ -d "$MODDIR/triango-bridge-lkm" ]; then
	cp -f "$MODDIR"/triango-bridge-lkm/* "$TRI/" 2>/dev/null
	chmod 755 "$TRI/triango_bridge.ko" "$TRI/triango-bridge" 2>/dev/null
fi

# Marker so the WebUI can tell the module ran.
date > "$TRI/log/post-fs-data.stamp" 2>/dev/null
