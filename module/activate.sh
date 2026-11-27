#!/system/bin/sh
# SPDX-License-Identifier: MIT
# Triango ARM32 Bridge — activation script (v1.2.2-b).
#
# Arms ro.dalvik.vm.native.bridge and restarts zygote ONCE, then WATCHES
# zygote for 60 s.  If zygote/system_server do not come back, every change
# is rolled back automatically and full logs are captured first — the device
# must NEVER be left without a zygote.
#
# v1.2.2-b (Android 17 root-cause fixes, verified against AOSP art:
#   GrapheneOS/platform_art branch 17, libnativebridge/native_bridge.cc):
#
#   1. NativeBridgeNameAcceptable() only accepts names matching
#      [a-zA-Z][a-zA-Z0-9._-]* — an absolute path ("/data/...") is REJECTED
#      before dlopen.  The prop is therefore the BARE filename and the .so is
#      resolved through the systemless mount /system/lib64 (module payload).
#      If the mount is missing (module installed but never fully rebooted),
#      activation is CANCELLED — never arm a prop that cannot resolve.
#
#   2. ART calls initialize()/getAppEnv() with the HOST ISA ("arm64" —
#      GetInstructionSetString(kRuntimeISA)); the fixed lib now accepts it
#      and gates guest code per-library in isSupported().
#
#   3. No more `stop; start` (legacy init builtins can leave the main class
#      down on modern Samsung init) — only targeted ctl.restart zygote[+_secondary].
#      No more ld.config.txt sed patch (the Android 17 file format is
#      section-based; the old flat-key sed was a silent no-op — removed to
#      eliminate any linker-config corruption risk).
#
# Prop changes are snapshotted before arming and restored on rollback.
#
# Usage: activate.sh [--no-restart]
#   --no-restart : arm the props but do not restart zygote (service.sh uses
#                  this when config/disable_auto_softreboot is present).
MODDIR=${0%/*}
TRI=/data/adb/triango
LIB=$TRI/lib
LOG=$TRI/log
CFG=$TRI/config
NB_NAME=libtriango_native_bridge.so
SYSNB=/system/lib64/$NB_NAME

mkdir -p "$LIB" "$LOG" "$CFG" 2>/dev/null

log() {
	echo "triango: $*" > /dev/kmsg 2>/dev/null || true
	echo "$(date '+%F %T') $*" >> "$LOG/activation.log" 2>/dev/null || true
}

RESTART=1
[ "$1" = "--no-restart" ] && RESTART=0

# ------------------------------------------------------------------ gates
[ -f "$CFG/safe_mode" ] && { log "activate: safe_mode present — cancelled"; exit 0; }
[ -f "$CFG/disable_nativebridge" ] && { log "activate: disable_nativebridge — cancelled"; exit 0; }

# ------------------------------------------- 1. systemless mount must exist
# Android 17 only accepts a bare bridge NAME (NativeBridgeNameAcceptable) and
# resolves it inside the system linker namespace — i.e. /system/lib64.  The
# module ships system/lib64/$NB_NAME (KernelSU/BakaSU magic mount at boot).
if [ ! -r "$SYSNB" ]; then
	log "activate: $SYSNB missing — module mounted? full reboot required; cancelled"
	echo "mount_missing $(date '+%F %T')" > "$LOG/activate_cancelled" 2>/dev/null
	exit 2
fi

# ------------------------------------------------------- 2. stage payloads
for f in libtriango_nb_engine.so libtriango_native_bridge.so; do
	[ -f "$MODDIR/$f" ] && cp -f "$MODDIR/$f" "$LIB/$f" 2>/dev/null
done

# ---------------------------------------------- 3. perms 0755 / 0644 (spec)
chmod 755 "$TRI" "$LIB" 2>/dev/null
for so in "$LIB"/*.so; do
	[ -f "$so" ] && chmod 644 "$so" 2>/dev/null
done

# -------------------------------- 4. SELinux system_file label gate (spec)
if command -v chcon >/dev/null 2>&1; then
	if ! chcon -R u:object_r:system_file:s0 "$LIB" 2>/dev/null; then
		log "activate: chcon system_file FAILED — cancelled (see log/chcon_failed)"
		echo "chcon_failed $(date '+%F %T')" > "$LOG/chcon_failed" 2>/dev/null
		exit 3
	fi
fi

# ------------------------------------------- 5. snapshot props (rollback)
SNAP=$LOG/pre_snapshot
: > "$SNAP" 2>/dev/null
for k in ro.dalvik.vm.native.bridge ro.product.cpu.abilist \
	ro.product.cpu.abilist32 ro.product.cpu.abilist64 \
	ro.dalvik.vm.isa.arm ro.dalvik.vm.isa.arm.variant \
	ro.dalvik.vm.isa.arm.features ro.enable.native.bridge.exec; do
	echo "$k $(getprop "$k")" >> "$SNAP" 2>/dev/null
done

# ------------------------------------------------------------- 6. the props
resetprop ro.dalvik.vm.isa.arm arm64
resetprop ro.dalvik.vm.isa.arm.variant generic
resetprop ro.dalvik.vm.isa.arm.features default
resetprop ro.enable.native.bridge.exec 1
resetprop ro.product.cpu.abilist "arm64-v8a,armeabi-v7a,armeabi"
resetprop ro.product.cpu.abilist32 "armeabi-v7a,armeabi"
resetprop ro.product.cpu.abilist64 "arm64-v8a"
# BARE filename — see header (Android 17 NativeBridgeNameAcceptable).
resetprop ro.dalvik.vm.native.bridge "$NB_NAME"
log "activate: props armed (native.bridge=$NB_NAME, mount=$SYSNB)"

# ------------------------------------------------- 7. restart + WATCHDOG
[ "$RESTART" = "0" ] && { log "activate: --no-restart — armed without zygote restart"; exit 0; }

ZP_BEFORE=$(pidof zygote64 2>/dev/null)

setprop ctl.restart zygote 2>/dev/null
if [ -n "$(getprop init.svc.zygote_secondary)" ]; then
	setprop ctl.restart zygote_secondary 2>/dev/null
fi
log "activate: zygote restart requested (was pid=${ZP_BEFORE:-none})"

# Phase 1: zygote64 must come back with a NEW pid (≤ 30 s).
ok=0
i=0
while [ "$i" -lt 12 ]; do
	sleep 2.5
	ZP=$(pidof zygote64 2>/dev/null)
	if [ -n "$ZP" ] && [ "$ZP" != "$ZP_BEFORE" ]; then
		ok=1
		break
	fi
	i=$((i + 1))
done

# Phase 2: system_server must be alive and stable (≤ 30 s more).
if [ "$ok" = "1" ]; then
	last=""
	stable=0
	i=0
	while [ "$i" -lt 12 ]; do
		sleep 2.5
		SP=$(pidof system_server 2>/dev/null)
		if [ -n "$SP" ]; then
			[ "$SP" = "$last" ] && stable=$((stable + 1)) || stable=0
			last=$SP
			[ "$stable" -ge 1 ] && break
		else
			stable=0
			last=""
		fi
		i=$((i + 1))
	done
	[ "$stable" -ge 1 ] || ok=0
fi

if [ "$ok" = "1" ]; then
	log "activate: HEALTHY zygote=$ZP system_server=${SP:-?}"
	echo "armed_ok zygote=$ZP system_server=${SP:-?} $(date '+%F %T')" > "$LOG/armed_ok" 2>/dev/null
	rm -f "$LOG/activate_cancelled" "$LOG/chcon_failed" 2>/dev/null
	exit 0
fi

# ------------------------------------------------ 8. ROLLBACK + log capture
TS=$(date +%Y%m%d-%H%M%S 2>/dev/null)
RD=$LOG/rollback-$TS
mkdir -p "$RD" 2>/dev/null
log "activate: WATCHDOG FAILED — capturing logs to $RD and rolling back"

logcat -d -b all > "$RD/logcat.txt" 2>/dev/null
dmesg > "$RD/dmesg.txt" 2>/dev/null
getprop > "$RD/getprop.txt" 2>/dev/null
ls -lZ "$LIB" > "$RD/state_lib.txt" 2>/dev/null
ls -lZ "$SYSNB" > "$RD/sysnb.txt" 2>/dev/null
cat "$SNAP" > "$RD/pre_snapshot.txt" 2>/dev/null
pidof zygote64 zygote zygote_secondary system_server > "$RD/pids.txt" 2>/dev/null
{ getprop init.svc.zygote; getprop init.svc.zygote_secondary; getprop init.svc.surfaceflinger; } \
	> "$RD/init_svc.txt" 2>/dev/null
grep -i "triango\|/system/lib64" /proc/mounts > "$RD/mounts.txt" 2>/dev/null
cat "$LOG/activation.log" > "$RD/activation.log" 2>/dev/null

# Restore every prop we changed to its pre-activation value.
while read -r k v; do
	[ -z "$k" ] && continue
	if [ -n "$v" ]; then
		resetprop "$k" "$v" 2>/dev/null
	else
		resetprop --delete "$k" 2>/dev/null
	fi
done < "$SNAP"

setprop ctl.restart zygote 2>/dev/null
if [ -n "$(getprop init.svc.zygote_secondary)" ]; then
	setprop ctl.restart zygote_secondary 2>/dev/null
fi

touch "$CFG/safe_mode" 2>/dev/null
echo "rollback_triggered $TS (logs: $RD)" > "$LOG/ROLLBACK_TRIGGERED" 2>/dev/null
log "activate: rollback complete — safe_mode set; device must recover without Triango"
exit 4
