#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build everything that can be built on this host:
#   1) bridge unit tests (host toolchain)
#   2) bridge binary (host or cross via CROSS_COMPILE)
#   3) module zip (kernel module included when already built)
#
# The kernel module itself needs a prepared kernel tree — see
# kernel/triango-bridge-lkm/build.sh (CI builds it automatically).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "== 1/3 bridge unit tests =="
make -C "$ROOT/bridge" test

echo "== 2/3 bridge binary =="
make -C "$ROOT/bridge" all

echo "== 3/3 module package =="
"$ROOT/tools/package-module.sh"

echo "all done."
