#!/system/bin/sh
# SPDX-License-Identifier: MIT
# Triango ARM32 Bridge — soft-reboot activator (v1.2.1-b).
#
#   su -c /data/adb/modules/triango_arm32_bridge/activate.sh [--no-restart]
#
# Activates (or re-activates) the Triango stack WITHOUT a full reboot.
# v1.2.1-b: ro.dalvik.vm.native.bridge is assigned ONLY here (and in
# service.sh) and only after all 4 readiness preconditions pass:
#   (a) bridge + engine .so staged under /data/adb/triango/lib
#   (b) permissions 0755 (dirs) / 0644 (.so)
#   (c) chcon u:object_r:system_file:s0 on the lib dir succeeded
#   (d) linkerconfig namespace knows /data/adb/triango/lib
# Anything less and the activation is cancelled — zygote must never see
# the prop before the libraries are loadable.
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

if [ -f "$TRI/config/safe_mode" ]; then
	log "SAFE MODE active ($TRI/config/safe_mode) — activation cancelled"
	exit 1
fi

# 0. Permissions (Fix 1) — 0755 dirs, 0644 .so.
chmod 755 "$TRI" "$TRI/lib" 2>/dev/null
for so in "$TRI/lib"/*.so; do
	[ -f "$so" ] && chmod 644 "$so" 2>/dev/null
done

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

	# 1b. Stage the interpreter engine plug-in.
	for f in "$MODDIR/libtriango_nb_engine.so" \
		"$MODDIR/system/lib64/libtriango_nb_engine.so"; do
		if [ -f "$f" ]; then
			cp -f "$f" "$TRI/lib/libtriango_nb_engine.so" 2>/dev/null
			chmod 644 "$TRI/lib/libtriango_nb_engine.so" 2>/dev/null
			log "engine plug-in staged"
			break
		fi
	done

	# 2. SELinux label (Fix 2) — zygote dlopen requires system_file.
	#    Failure cancels the activation entirely.
	if chcon -R u:object_r:system_file:s0 "$TRI/lib" 2>/dev/null; then
		rm -f "$TRI/log/chcon_failed" 2>/dev/null
		log "lib relabeled system_file"
	else
		touch "$TRI/log/chcon_failed" 2>/dev/null
		log "ERROR: chcon system_file failed — native.bridge activation cancelled"
		exit 1
	fi

	# 3. Linker namespace permission (idempotent) — precondition (d).
	LC=/linkerconfig/ld.config.txt
	if [ -f "$LC" ] && ! grep -q "data/adb/triango/lib" "$LC" 2>/dev/null; then
		sed -i 's#namespace.default.search.paths = #namespace.default.search.paths = /data/adb/triango/lib: #' "$LC" 2>/dev/null
		sed -i 's#namespace.default.permitted.paths = #namespace.default.permitted.paths = /data/adb/triango/lib: #' "$LC" 2>/dev/null
		log "linkerconfig patched"
	fi

	# 4. Readiness preconditions before the prop goes live.
	ok=1
	[ -f "$TRI/lib/libtriango_native_bridge.so" ] || ok=0
	[ -f "$TRI/lib/libtriango_nb_engine.so" ] || log "WARN: engine .so not staged (bridge will report no-engine)"
	[ -d "$TRI/lib" ] || ok=0
	[ -f "$TRI/log/chcon_failed" ] && ok=0
	grep -q "data/adb/triango/lib" /linkerconfig/ld.config.txt 2>/dev/null || {
		[ -f /linkerconfig/ld.config.txt ] && ok=0
	}
	if [ "$ok" = 1 ]; then
		# Native bridge prop (absolute path preferred).
		"$RESETPROP" ro.dalvik.vm.native.bridge \
			"$TRI/lib/libtriango_native_bridge.so" 2>/dev/null
		log "native.bridge armed: $TRI/lib/libtriango_native_bridge.so"
	else
		log "ERROR: readiness preconditions failed — native.bridge NOT set"
		exit 1
	fi
fi

# 5. ABI + ART ISA props (Fix 3).
if [ ! -f "$TRI/config/disable_abilist32" ]; then
	"$RESETPROP" ro.product.cpu.abilist "arm64-v8a,armeabi-v7a,armeabi" 2>/dev/null
	"$RESETPROP" ro.product.cpu.abilist32 "armeabi-v7a,armeabi" 2>/dev/null
	"$RESETPROP" ro.product.cpu.abilist64 "arm64-v8a" 2>/dev/null
	"$RESETPROP" ro.vendor.product.cpu.abilist "arm64-v8a,armeabi-v7a,armeabi" 2>/dev/null
	"$RESETPROP" ro.vendor.product.cpu.abilist32 "armeabi-v7a,armeabi" 2>/dev/null
fi
"$RESETPROP" ro.dalvik.vm.isa.arm "arm64" 2>/dev/null
"$RESETPROP" ro.dalvik.vm.isa.arm.variant "generic" 2>/dev/null
"$RESETPROP" ro.dalvik.vm.isa.arm.features "default" 2>/dev/null
"$RESETPROP" ro.enable.native.bridge.exec "1" 2>/dev/null

if [ "$1" = "--no-restart" ]; then
	log "props applied (zygote restart skipped)"
	exit 0
fi

# 6. Soft reboot: restart the Android framework so zygote re-reads props.
log "soft reboot: restarting zygote"
setprop ctl.restart zygote 2>/dev/null
setprop ctl.restart zygote_secondary 2>/dev/null
# Fallback for older init:
stop 2>/dev/null
start 2>/dev/null
log "soft reboot requested"
