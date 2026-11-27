#!/system/bin/sh
# SPDX-License-Identifier: MIT
# Triango ARM32 Bridge — install-time hook (KernelSU/Magisk, v1.0).
#
# Stages the bridge library into /data/adb/triango/lib so that the WebUI's
# soft-reboot activation works WITHOUT a full reboot right after install.
SKIPUNZIP=0
TRI=/data/adb/triango

ui_print "- Triango ARM32 Bridge v1.0.0"
ui_print "- staging state under /data/adb/triango"

mkdir -p "$TRI/lib" "$TRI/log" "$TRI/config" "$TRI/engines" "$TRI/sysroot32"

# Stage the prebuilt bridge library when the zip carries one.
for f in "$MODPATH/system/lib64/libtriango_native_bridge.so" \
	"$MODPATH/libtriango_native_bridge.so"; do
	if [ -f "$f" ]; then
		cp -f "$f" "$TRI/lib/libtriango_native_bridge.so"
		chmod 644 "$TRI/lib/libtriango_native_bridge.so"
		ui_print "- libtriango_native_bridge.so staged (soft reboot ready)"
		break
	fi
done
if [ ! -f "$TRI/lib/libtriango_native_bridge.so" ]; then
	ui_print "! libtriango_native_bridge.so not in zip"
	ui_print "! build it: tools/build-nativebridge.sh <ndk>"
	ui_print "! (activation still fixes ABIs + props)"
fi

ui_print "- done. Activate via the module WebUI (soft reboot) or"
ui_print "  su -c /data/adb/modules/triango_arm32_bridge/activate.sh"
