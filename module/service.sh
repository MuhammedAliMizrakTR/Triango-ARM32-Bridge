#!/system/bin/sh
# SPDX-License-Identifier: MIT
# Triango ARM32 Bridge — late_start service (v1.2.2-b).
#
# LateLoad-compatible: runs after boot_completed (BakaSU ksud late-load may
# schedule this even later).  Everything is idempotent.
#
# v1.2.2-b: all arming logic lives in activate.sh (including the 60 s zygote
# watchdog and automatic rollback with log capture).  This wrapper only
# handles boot sequencing:
#   - waits for boot_completed
#   - resets the failed-boot counter on a healthy boot
#   - honours config/{safe_mode,disable_nativebridge,disable_auto_softreboot}
#   - runs activate.sh (full flow) — or with --no-restart when the user
#     disabled the automatic zygote restart.
MODDIR=${0%/*}
TRI=/data/adb/triango
LOG=$TRI/log
CFG=$TRI/config

mkdir -p "$LOG" "$CFG" 2>/dev/null

log() {
	echo "triango: $*" > /dev/kmsg 2>/dev/null || true
}

# Wait for the boot to settle (max ~2 min).
i=0
while [ "$(getprop sys.boot_completed)" != "1" ] && [ "$i" -lt 240 ]; do
	sleep 1
	i=$((i + 1))
done

# Boot reached userspace: clear the crash-loop counter.
if [ "$(getprop sys.boot_completed)" = "1" ]; then
	echo 0 > "$LOG/boot_attempts" 2>/dev/null
fi

[ -f "$CFG/safe_mode" ] && { log "service: safe_mode — not arming"; exit 0; }
[ -f "$CFG/disable_nativebridge" ] && { log "service: disable_nativebridge — not arming"; exit 0; }

if [ -f "$CFG/disable_auto_softreboot" ]; then
	log "service: arming without zygote restart (disable_auto_softreboot)"
	sh "$MODDIR/activate.sh" --no-restart >/dev/null 2>&1
else
	log "service: arming via activate.sh (watchdog + auto-rollback)"
	sh "$MODDIR/activate.sh" >/dev/null 2>&1
	rc=$?
	case "$rc" in
		0) log "service: armed and healthy" ;;
		2) log "service: systemless mount missing — full reboot required" ;;
		3) log "service: chcon failed — activation cancelled" ;;
		4) log "service: WATCHDOG ROLLBACK — logs in $LOG/rollback-*, safe_mode set" ;;
		*) log "service: activate.sh exit $rc" ;;
	esac
fi

# Optional GPU thunk daemon.
if [ ! -f "$CFG/disable_gpud" ] && [ -x "$TRI/triango-gpud" ]; then
	"$TRI/triango-gpud" >/dev/null 2>&1 &
fi
