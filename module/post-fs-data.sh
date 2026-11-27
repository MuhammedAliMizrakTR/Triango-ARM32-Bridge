#!/system/bin/sh
# Triango ARM32 Bridge — KernelSU/BakaSU module setup (post-fs-data stage).
#
# Idempotent by boot id: safe to run from both post-fs-data.sh and service.sh,
# which matters in BakaSU LKM jailbreak mode where ksud late-load may invoke
# the stages out of order. See docs/bakasu-lkm-jailbreak.md.
MODDIR=${0%/*}
TRI=/data/adb/triango
LOG="$TRI/triango.log"

log() {
	echo "$(date '+%F %T') [setup] $*" >> "$LOG" 2>/dev/null
}

BOOT_ID=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)
if [ -f "$TRI/.setup_done" ] && \
   [ "$(cat "$TRI/.setup_done" 2>/dev/null)" = "$BOOT_ID" ]; then
	exit 0
fi

mkdir -p "$TRI/engines" "$TRI/sysroot32" 2>/dev/null
chmod 700 "$TRI" 2>/dev/null

# Stage module payloads into /data/adb/triango.
for f in triango-bridge triango-bridge-lkm triango_bridge.ko; do
	[ -f "$MODDIR/$f" ] && cp -f "$MODDIR/$f" "$TRI/$f" 2>/dev/null
done
chmod 755 "$TRI/triango-bridge" "$TRI/triango-bridge-lkm" 2>/dev/null
chmod 644 "$TRI/triango_bridge.ko" 2>/dev/null

STATE=0 # 0=none 1=lkm 2=binfmt_misc

# 1) Preferred: the kernel LKM (binfmt interception in-kernel).
if [ -f "$TRI/triango_bridge.ko" ] && [ -f "$TRI/triango-bridge" ]; then
	if insmod "$TRI/triango_bridge.ko" \
		"bridge_path=$TRI/triango-bridge-lkm" >/dev/null 2>&1; then
		STATE=1
		log "triango_bridge.ko loaded (bridge_path=$TRI/triango-bridge-lkm)"
	else
		log "insmod failed (vermagic/ABI mismatch? see dmesg)"
	fi
fi

# 2) Fallback: binfmt_misc registration. The 'P' flag yields the exact same
#    argv contract as the LKM ([interp, path, orig_argv0, orig_args...]).
if [ "$STATE" = "0" ]; then
	BM=/proc/sys/fs/binfmt_misc
	[ -d "$BM" ] || mount -t binfmt_misc binfmt_misc "$BM" 2>/dev/null
	if [ -d "$BM" ]; then
		if [ -e "$BM/triango-arm32" ]; then
			STATE=2
		else
			cat > "$BM/register" 2>/dev/null <<EOF
:triango-arm32:M::\x7fELF\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x28\x00:\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xff\xff:$TRI/triango-bridge-lkm:P
EOF
			if [ -e "$BM/triango-arm32" ]; then
				STATE=2
				log "binfmt_misc fallback registered (P flag)"
			fi
		fi
	fi
fi

if [ "$STATE" = "0" ]; then
	log "WARNING: neither the LKM nor binfmt_misc is active — arm32 exec will not be intercepted"
fi

echo "$BOOT_ID" > "$TRI/.setup_done" 2>/dev/null
exit 0
