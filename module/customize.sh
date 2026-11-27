#!/system/bin/sh
# SPDX-License-Identifier: MIT
# Triango ARM32 Bridge — install-time hook (KernelSU/Magisk, v1.0).
#
# Stages the bridge library into /data/adb/triango/lib so that the WebUI's
# soft-reboot activation works WITHOUT a full reboot right after install.
SKIPUNZIP=0
TRI=/data/adb/triango

ui_print "- Triango ARM32 Bridge v1.1.0-a"
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

# Stage the interpreter engine plug-in when the zip carries one.
for f in "$MODPATH/libtriango_nb_engine.so" \
	"$MODPATH/system/lib64/libtriango_nb_engine.so"; do
	if [ -f "$f" ]; then
		cp -f "$f" "$TRI/lib/libtriango_nb_engine.so"
		chmod 644 "$TRI/lib/libtriango_nb_engine.so"
		ui_print "- libtriango_nb_engine.so staged (V1.1(a) engine)"
		break
	fi
done
if [ ! -f "$TRI/lib/libtriango_nb_engine.so" ]; then
	ui_print "! libtriango_nb_engine.so not in zip"
	ui_print "! build it: make -C nativebridge engine (or NDK build)"
fi

# Apply the ABI advertisement NOW so arm32 APKs install straight from the
# package installer in this very boot (no reboot needed for INSTALL).
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
		ui_print "- abilist32 advertised now (package installer ready)"
	else
		ui_print "! resetprop not found; abilist set at next boot (system.prop)"
	fi
fi

ui_print "- done. Activate via the module WebUI (soft reboot) or"
ui_print "  su -c /data/adb/modules/triango_arm32_bridge/activate.sh"
