# triango_bridge.ko — Arm32 exec interception LKM

High-priority `linux_binfmt` handler that redirects **ELF32 / EM_ARM**
executables to the Triango userspace bridge (`triango-bridge`, AArch64).

- License: **GPL-2.0-only**
- Target: GKI **android16-6.12** (Samsung Galaxy S26 Ultra kernel
  `6.12.30-android16-5-…-4k`) and other 6.12+ Android GKI trees.
- Uses only `EXPORT_SYMBOL()` interfaces (`__register_binfmt`,
  `copy_string_kernel`, `bprm_change_interp`, `open_exec`) — GKI-ABI friendly.

## Build

```bash
# Kernel tree (GKI android16-6.12), prepared for modules:
git clone --depth=1 -b android16-6.12 \
    https://android.googlesource.com/kernel/common kernel-common
make -C kernel-common ARCH=arm64 defconfig
make -C kernel-common ARCH=arm64 modules_prepare

./build.sh ../../kernel-common
```

CI builds the module on every push (`.github/workflows/ci.yml`).

## Load

```bash
su -c 'insmod triango_bridge.ko bridge_path=/data/adb/triango/triango-bridge'
su -c 'dmesg | grep triango'
```

## Parameters

| Param | Default | Meaning |
|---|---|---|
| `bridge_path` | `/data/adb/triango/triango-bridge` | AArch64 bridge binary |
| `enabled` | `1` | master switch |
| `android_only` | `1` | only intercept `PT_INTERP=/system/bin/linker` binaries |
| `verbose` | `0` | verbose logging |

See `docs/architecture.md` (§2–3) for the exec flow and argv contract.
