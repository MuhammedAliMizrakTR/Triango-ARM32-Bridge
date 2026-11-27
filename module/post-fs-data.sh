#!/system/bin/sh
# SPDX-License-Identifier: MIT
# Triango ARM32 Bridge — post-fs-data (v1.2.1-b).
#
# Runs BEFORE system_server caches Build.SUPPORTED_ABIS and before zygote
# reads ro.dalvik.vm.native.bridge.  Everything here is idempotent and
# LateLoad-safe (BakaSU ksud late-load may re-run service parts later).
#
# v1.2.1-b zygote-safety contract:
#   - state dir is world-readable (0755) so the zygote/app domains can reach
#     the staged .so files (0700 caused EACCES dlopen failures);
#   - lib files are relabeled system_file after staging (chcon) — the raw
#     adb_data_file label makes zygote's dlopen() fail on Android 17;
#   - ro.dalvik.vm.native.bridge is NOT set here anymore.  It is assigned
#     only by activate.sh/service.sh after the 4 readiness preconditions,
#     so zygote can never see the prop before the libraries are staged.
MODDIR=${0%/*}
TRI=/data/adb/triango

log() {
	echo "triango: $*" > /dev/kmsg 2>/dev/null || true
}

mkdir -p "$TRI/engines" "$TRI/sysroot32" "$TRI/lib" "$TRI/log" "$TRI/config"

# ---- 0. Boot counter / safe mode (crash-loop guard) ----
# Each boot increments the counter; service.sh clears it once
# boot_completed is reached.  Three consecutive incomplete boots put the
# module into safe mode: native.bridge activation is skipped entirely.
ATTEMPTS="$TRI/log/boot_attempts"
n=$(cat "$ATTEMPTS" 2>/dev/null)
case "$n" in
	''|*[!0-9]*) n=0 ;;
esac
n=$((n + 1))
echo "$n" > "$ATTEMPTS" 2>/dev/null
if [ "$n" -ge 3 ] && [ ! -f "$TRI/config/safe_mode" ]; then
	touch "$TRI/config/safe_mode" 2>/dev/null
	log "SAFE MODE: $n incomplete boots — native.bridge disabled (remove $TRI/config/safe_mode to re-enable)"
fi

# ---- 1. Permissions (zygote/app must be able to traverse + read) ----
# Fix 1 (v1.2.1-b): 0700 on $TRI cut off every non-root dlopen with EACCES.
chmod 755 "$TRI" 2>/dev/null
chmod 755 "$TRI/lib" 2>/dev/null
chmod 755 "$TRI/engines" "$TRI/sysroot32" "$TRI/log" 2>/dev/null
chmod 700 "$TRI/config" 2>/dev/null

# ---- 2. ABI advertising (INSTALL_FAILED_NO_MATCHING_ABIS fix) ----
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

# ---- 3. NativeBridge staging (no prop yet — see header) ----
if [ ! -f "$TRI/config/disable_nativebridge" ]; then
	# Stage (or refresh) the bridge .so into /data/adb/triango/lib so the
	# soft-reboot activator (and zygote via linkerconfig) can reach it.
	for f in "$MODDIR/system/lib64/libtriango_native_bridge.so" \
		"$MODDIR/libtriango_native_bridge.so"; do
		if [ -f "$f" ]; then
			cp -f "$f" "$TRI/lib/libtriango_native_bridge.so" 2>/dev/null
			break
		fi
	done

	# Stage the interpreter engine plug-in alongside the bridge.
	for f in "$MODDIR/libtriango_nb_engine.so" \
		"$MODDIR/system/lib64/libtriango_nb_engine.so"; do
		if [ -f "$f" ]; then
			cp -f "$f" "$TRI/lib/libtriango_nb_engine.so" 2>/dev/null
			log "engine plug-in staged"
			break
		fi
	done

	# Fix 1: every staged .so must be world-readable (0644).
	for so in "$TRI/lib"/*.so; do
		[ -f "$so" ] && chmod 644 "$so" 2>/dev/null
	done

	# Fix 2: relabel for zygote/app dlopen.  Failure cancels later
	# native.bridge activation (the caller re-checks this marker).
	if chcon -R u:object_r:system_file:s0 "$TRI/lib" 2>/dev/null; then
		rm -f "$TRI/log/chcon_failed" 2>/dev/null
		log "lib relabeled system_file"
	else
		touch "$TRI/log/chcon_failed" 2>/dev/null
		log "WARN: chcon system_file failed — native.bridge activation cancelled"
	fi

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
		# Fix 3: Android 17 ART mandatory ISA declarations (these must
		# be live before zygote reads them; unlike native.bridge they are
		# safe to publish early).
		"$RESETPROP" ro.dalvik.vm.isa.arm "arm64" 2>/dev/null
		"$RESETPROP" ro.dalvik.vm.isa.arm.variant "generic" 2>/dev/null
		"$RESETPROP" ro.dalvik.vm.isa.arm.features "default" 2>/dev/null
		"$RESETPROP" ro.enable.native.bridge.exec "1" 2>/dev/null
		log "ART ISA props staged"
	fi
	# NOTE: ro.dalvik.vm.native.bridge is intentionally NOT set here.
	# activate.sh / service.sh assign it after validating readiness.
fi

# Stage module payloads into /data/adb/triango.
if [ -d "$MODDIR/triango-bridge-lkm" ]; then
	cp -f "$MODDIR"/triango-bridge-lkm/* "$TRI/" 2>/dev/null
	chmod 755 "$TRI/triango_bridge.ko" "$TRI/triango-bridge" 2>/dev/null
fi

# Marker so the WebUI can tell the module ran.
date > "$TRI/log/post-fs-data.stamp" 2>/dev/null
