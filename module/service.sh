#!/system/bin/sh
# Triango ARM32 Bridge — service stage.
#
# Safety net for BakaSU LKM jailbreak mode (ksud late-load): if the
# post-fs-data stage did not run for this boot, run the setup now.
MODDIR=${0%/*}
TRI=/data/adb/triango
LOG="$TRI/triango.log"

BOOT_ID=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)
if [ ! -f "$TRI/.setup_done" ] || \
   [ "$(cat "$TRI/.setup_done" 2>/dev/null)" != "$BOOT_ID" ]; then
	sh "$MODDIR/post-fs-data.sh"
fi

# Record a status snapshot for debugging (su context, best effort).
if [ -x "$TRI/triango-bridge" ]; then
	{
		echo "$(date '+%F %T') [service] status:"
		"$TRI/triango-bridge" --probe
	} >> "$LOG" 2>&1
fi
exit 0
