#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Package the KernelSU/BakaSU module zip: dist/triango-arm32-bridge-ksu.zip
#
# Payloads (built beforehand or provided via env):
#   TRIANGO_BRIDGE  - AArch64 bridge binary (default: bridge/build/triango-bridge)
#   TRIANGO_KO      - kernel module             (default: kernel/triango-bridge-lkm/triango_bridge.ko)
#   TRIANGO_NB_LIB  - NativeBridge library      (default: nativebridge/prebuilt/libtriango_native_bridge.so)
#   TRIANGO_ENGINE  - interpreter engine plug-in (default: nativebridge/build/libtriango_nb_engine.so)
#   TRIANGO_GPUD    - GPU thunk daemon          (default: gpu/build/triango-gpud)
# Missing payloads are skipped with a warning (the module then relies on the
# binfmt_misc fallback / manual deployment).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="$ROOT/build/module-zip"
DIST="$ROOT/dist"
BRIDGE="${TRIANGO_BRIDGE:-$ROOT/bridge/build/triango-bridge}"
KO="${TRIANGO_KO:-$ROOT/kernel/triango-bridge-lkm/triango_bridge.ko}"
NBLIB="${TRIANGO_NB_LIB:-$ROOT/nativebridge/prebuilt/libtriango_native_bridge.so}"
ENGINE="${TRIANGO_ENGINE:-$ROOT/nativebridge/build/libtriango_nb_engine.so}"
GPUD="${TRIANGO_GPUD:-$ROOT/gpu/build/triango-gpud}"

VERSION="$(sed -n 's/^version=//p' "$ROOT/module/module.prop")"
OUT="$DIST/triango-arm32-bridge-ksu.zip"

rm -rf "$STAGE"
mkdir -p "$STAGE" "$DIST"
cp -a "$ROOT/module/." "$STAGE/"
rm -f "$STAGE/README.md"

if [[ -f "$BRIDGE" ]]; then
	cp "$BRIDGE" "$STAGE/triango-bridge"
else
	echo "warning: bridge binary not found at '$BRIDGE' — packaging without it" >&2
fi
if [[ -f "$KO" ]]; then
	cp "$KO" "$STAGE/triango_bridge.ko"
else
	echo "warning: kernel module not found at '$KO' — packaging without it (binfmt_misc fallback only)" >&2
fi

# NativeBridge library -> system/lib64 (magic mount) + plain copy for the
# soft-reboot activator (customize.sh stages it into /data/adb/triango/lib).
if [[ -f "$NBLIB" ]]; then
	mkdir -p "$STAGE/system/lib64"
	cp "$NBLIB" "$STAGE/system/lib64/libtriango_native_bridge.so"
	cp "$NBLIB" "$STAGE/libtriango_native_bridge.so"
else
	echo "warning: NativeBridge library not found at '$NBLIB' — build with tools/build-nativebridge.sh <ndk>" >&2
fi

# Interpreter engine plug-in (V1.1(a)) -> /data/adb/triango/lib staging.
if [[ -f "$ENGINE" ]]; then
	cp "$ENGINE" "$STAGE/libtriango_nb_engine.so"
else
	echo "warning: engine plug-in not found at '$ENGINE' — build with make -C nativebridge engine" >&2
fi

# GPU thunk daemon.
if [[ -f "$GPUD" ]]; then
	cp "$GPUD" "$STAGE/triango-gpud"
else
	echo "warning: triango-gpud not found at '$GPUD' — GPU thunk mode unavailable" >&2
fi

chmod 755 "$STAGE/post-fs-data.sh" "$STAGE/service.sh" "$STAGE/uninstall.sh" \
	"$STAGE/activate.sh" "$STAGE/customize.sh" \
	"$STAGE/triango-bridge-lkm" 2>/dev/null || true
[[ -f "$STAGE/triango-bridge" ]] && chmod 755 "$STAGE/triango-bridge"

rm -f "$OUT"
if command -v zip >/dev/null 2>&1; then
	(cd "$STAGE" && zip -qr "$OUT" .)
else
	python3 - "$STAGE" "$OUT" <<'PY'
import os, sys, zipfile
stage, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk(stage):
        for f in sorted(files):
            p = os.path.join(root, f)
            z.write(p, os.path.relpath(p, stage))
PY
fi

echo "packaged: $OUT ($VERSION)"
