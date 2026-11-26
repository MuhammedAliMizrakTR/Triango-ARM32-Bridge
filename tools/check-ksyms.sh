#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Verify that a connected device (via adb) exports the kernel symbols the
# triango_bridge.ko module links against. All five must be visible in
# /proc/kallsyms for the module to load on a GKI kernel.
#
#   ./tools/check-ksyms.sh          # uses 'adb shell su -c ...'
#   ./tools/check-ksyms.sh --kallsyms /path/to/kallsyms.txt   # offline file
set -euo pipefail

SYMS=(__register_binfmt unregister_binfmt copy_string_kernel bprm_change_interp open_exec)

get_kallsyms() {
	if [[ "${1:-}" == "--kallsyms" && -n "${2:-}" ]]; then
		cat "$2"
	else
		adb shell su -c 'cat /proc/kallsyms'
	fi
}

OUT="$(get_kallsyms "${1:-}" "${2:-}")"
missing=0
for s in "${SYMS[@]}"; do
	if grep -qE "[[:space:]]${s}$" <<<"$OUT"; then
		echo "OK      $s"
	else
		echo "MISSING $s"
		missing=1
	fi
done

if [[ "$missing" == "1" ]]; then
	echo "result: FAIL — the LKM will not link; the binfmt_misc fallback will be used" >&2
	exit 1
fi
echo "result: PASS — all required symbols are exported"
