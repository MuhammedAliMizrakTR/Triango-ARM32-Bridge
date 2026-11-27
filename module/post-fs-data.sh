#!/system/bin/sh
# SPDX-License-Identifier: MIT
# Triango ARM32 Bridge — post-fs-data (v1.2.2-b).
#
# Runs BEFORE zygote.  Only boot-time preparation lives here:
#   - state dirs with zygote-safe traversal perms (0755), .so 0644
#   - SELinux system_file relabel of the state libs (activation gate)
#   - boot crash-loop counter -> config/safe_mode after 3 failed boots
#   - system.prop (abilist32 + ISA props) is applied by KernelSU at this stage
#
# v1.2.2-b: the ld.config.txt sed patch has been REMOVED.  The bridge .so is
# resolved through the systemless mount /system/lib64 (module payload) and
# Android 17's NativeBridgeNameAcceptable() only accepts bare filenames, so
# the linker-config patch was a silent no-op at best — and a corruption risk
# at worst.  Nothing here may break zygote startup.
#
# ro.dalvik.vm.native.bridge is NEVER set here (see activate.sh).
MODDIR=${0%/*}
TRI=/data/adb/triango
LIB=$TRI/lib
LOG=$TRI/log
CFG=$TRI/config

mkdir -p "$TRI" "$LIB" "$LOG" "$CFG" 2>/dev/null

# --- Fix 2: state dir traversal perms (0755) + .so 0644 -----------------
chmod 755 "$TRI" "$LIB" "$LOG" "$CFG" 2>/dev/null
for so in "$LIB"/*.so; do
	[ -f "$so" ] && chmod 644 "$so" 2>/dev/null
done

# --- Fix 3: SELinux system_file relabel of the staged libs ---------------
if command -v chcon >/dev/null 2>&1; then
	chcon -R u:object_r:system_file:s0 "$LIB" 2>/dev/null
fi

# --- Fix 4a: boot crash-loop counter -> safe mode ------------------------
n=$(cat "$LOG/boot_attempts" 2>/dev/null || echo 0)
case "$n" in ''|*[!0-9]*) n=0 ;; esac
n=$((n + 1))
echo "$n" > "$LOG/boot_attempts" 2>/dev/null
if [ "$n" -ge 3 ] && [ ! -f "$CFG/safe_mode" ]; then
	touch "$CFG/safe_mode" 2>/dev/null
	echo "safe_mode_after_${n}_boots $(date '+%F %T')" > "$LOG/SAFE_MODE" 2>/dev/null
	echo "triango: 3 failed boots — SAFE MODE, native bridge stays off" > /dev/kmsg 2>/dev/null
fi

exit 0
