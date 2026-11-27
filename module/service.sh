#!/system/bin/sh
# SPDX-License-Identifier: MIT
# Triango ARM32 Bridge — late_start service (v1.2.1-b).
#
# LateLoad-compatible: runs after boot_completed (BakaSU ksud late-load may
# schedule this even later).  Everything is idempotent — it is safe to run
# the module's activate script repeatedly.
#
# v1.2.1-b: this is one of the ONLY two places allowed to assign
# ro.dalvik.vm.native.bridge (see activate.sh).  The prop is set strictly
# after the 4 readiness preconditions pass, and the first boot that arms it
# performs a soft reboot so zygote picks it up cleanly.
MODDIR=${0%/*}
TRI=/data/adb/triango

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
	echo 0 > "$TRI/log/boot_attempts" 2>/dev/null
fi

if [ -f "$TRI/config/safe_mode" ]; then
	log "SAFE MODE active — service keeps props only, no native.bridge"
fi

# Fix 1 + Fix 2: permissions and SELinux label (idempotent).
chmod 755 "$TRI" "$TRI/lib" 2>/dev/null
for so in "$TRI/lib"/*.so; do
	[ -f "$so" ] && chmod 644 "$so" 2>/dev/null
done
if chcon -R u:object_r:system_file:s0 "$TRI/lib" 2>/dev/null; then
	rm -f "$TRI/log/chcon_failed" 2>/dev/null
else
	touch "$TRI/log/chcon_failed" 2>/dev/null
	log "WARN: chcon system_file failed"
fi

# Re-apply props + staging without bouncing zygote (it is up by now):
# sh "$MODDIR/activate.sh" --no-restart handles staging, labels, linkerconfig
# and — critically — the readiness-gated ro.dalvik.vm.native.bridge.
if [ ! -f "$TRI/config/safe_mode" ]; then
	prev=$(getprop ro.dalvik.vm.native.bridge)
	if sh "$MODDIR/activate.sh" --no-restart >/dev/null 2>&1; then
		now=$(getprop ro.dalvik.vm.native.bridge)
		# Fix 4: first boot that arms the prop gets one automatic soft
		# reboot so zygote loads the bridge without user action.
		if [ -n "$now" ] && [ "$prev" != "$now" ] && \
		   [ ! -f "$TRI/config/disable_auto_softreboot" ]; then
			log "native.bridge armed at runtime — soft reboot for zygote"
			setprop ctl.restart zygote 2>/dev/null
			setprop ctl.restart zygote_secondary 2>/dev/null
			stop 2>/dev/null
			start 2>/dev/null
		fi
	else
		log "activate.sh reported failure (preconditions) — zygote left untouched"
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
