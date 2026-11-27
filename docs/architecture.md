# Triango — Mimari / Architecture

Bu belge Triango ARM32 Bridge'in bileşenlerini, exec akışını ve tasarım kararlarını tanımlar.
(EN summary in each section header.)

## 1. Katmanlar / Layers

| Katman | Bileşen | Çalışma alanı | Rol |
|---|---|---|---|
| L0 | `triango_bridge.ko` | kernel (LKM) | ELF32/EM_ARM `execve` yakalama, köprüyü yorumlayıcı olarak devreye sokma |
| L1 | `triango-bridge` | userspace (AArch64) | ELF doğrulama, argv sözleşmesi çözümü, sysroot32 hazırlama, motor seçimi |
| L2 | motor (`qemu-arm`, ileride `triango-jit`) | userspace | AArch32 talimatlarını AArch64 üzerinde yürütme |
| L3 | `module/` (KernelSU/BakaSU) | root modül sistemi | Kurulum, kalıcılık, SELinux, binfmt_misc yedeği, prop yönetimi |

## 2. Exec akışı / Exec flow

`execve("/data/.../oyun32", ["oyun32","-x"], env)` çağrısında (hedef ELF32/EM_ARM ise):

1. `search_binary_handler()` binfmt listesini baştan tarar. Triango handler `insert_binfmt()` ile **listenin başına** kayıtlıdır → `binfmt_elf`'den (ve compat yoldan) **önce** denenir. (Kernel v6.12 davranışları `fs/binfmt_script.c` / `fs/binfmt_misc.c` ile birebir doğrulanmıştır.)
2. Handler `bprm->buf` üzerinde ELF kimliğini doğrular:
   - `ELFMAG`, `ELFCLASS32`, `ELFDATA2LSB`, `ET_EXEC|ET_DYN`, `EM_ARM`
   - `android_only=1` ise: program başlıklarında `PT_INTERP == "/system/bin/linker"` aranır (Android arm32); bulunamazsa `-ENOEXEC` (listeye devam).
3. Yeni argv **tersten** itilir (çekirdek argüman yığını ters yönde büyür; `fs/binfmt_script.c`'deki "done in reverse order" kuralı):
   - `copy_string_kernel(bprm->interp)` → **argv[1]** = orijinal hedef yolu
   - `copy_string_kernel(bridge_path)` → **argv[0]** = `triango-bridge`
   - `bprm->argc += 2`, `bprm_change_interp(bridge_path)`
4. `bprm->interpreter = open_exec(bridge_path)` ve `return 0`. Çekirdek yorumlayıcıyı (köprü) kendisi yükler — handler içinde recursive arama **yok** (v6.12 çekirdek sözleşmesi).
5. `triango-bridge` uyanır, hedefi yeniden okur ve motoru başlatır (kendini `execv` ile değiştirir).

### 2.1 argv sözleşmeleri / argv conventions

| Kaynak | bridge'in gördüğü argv |
|---|---|
| **LKM** (`triango_bridge.ko`) | `[bridge, target, orig_argv0, orig_args…]` |
| binfmt_misc yedeği (**P** bayrağı) | aynı biçim — P, orijinal argv[0]'ı korur (kernel dokümanı ile doğrulandı) |
| elle çağırma (varsayılan `manual`) | `[bridge, target, guest_args…]` |

Ayrım: resmi çağırıcıların ikisi de `triango-bridge-lkm` sarmalayıcısını kullanır ve
`--conv=lkm` geçer; elle kullanımda varsayılan `manual`'dir (`TRIANGO_ARGV_CONV` ile
zorlanabilir). `orig_argv0` bilerek **silinmez** (`remove_arg_zero` çağrılmaz — P bayrağı
ile aynı davranış); köprü onu motora `qemu-arm -0` ile aktarır.

## 3. L0 — `triango_bridge.ko`

Tek kaynak: `kernel/triango-bridge-lkm/triango_binfmt.c`.

**Tasarım kararları:**
- Yalnızca **export edilmiş** semboller kullanılır (`__register_binfmt`, `copy_string_kernel`, `bprm_change_interp`, `open_exec`, `kernel_read`) — GKI ABI uyumlu, kallsyms/kprobe numarası yok. Doğrulama: `tools/check-ksyms.sh`.
- Mekanikler Linux **v6.12** `binfmt_script`/`binfmt_misc` ile birebir aynıdır (kaynak karşılaştırılarak yazıldı).
- Modül parametreleri:
  - `bridge_path` (charp) — köprü yolu, varsayılan `/data/adb/triango/triango-bridge`
  - `enabled` (bool, 1)
  - `android_only` (bool, 1) — sadece Android linker'lı arm32 ELF'leri yakala
  - `verbose` (bool, 0)
- Yüklenemezse/çalışmazsa **L3'teki binfmt_misc yedeği** devreye girer (kullanıcı alanı kaydı, `P` bayraklı).

**Güvenlik sınırı:** modül `exec` yolunu değiştirir; bunu yalnızca root kurabilir. `android_only=1` varsayılanı, Linux arm32 statik ikili dosyalarının yanlışlıkla ele geçirilmesini engeller.

## 4. L1 — `triango-bridge` (kullanıcı alanı)

Taşınabilir C11; x86_64 host'ta derlenip test edilebilir (`make -C bridge test`).

- `src/elf32.c` — ELF32 ARM ayrıştırıcı/doğrulayıcı (`tb_elf32_parse`).
- `src/syscalls.c` — **Arm32 EABI → AArch64 syscall eşleme tablosu** (`tb_syscall_table`): isim, hedef nr, düzeltme bayrakları (`OPENAT`, `MMAP2`, `TIMESPEC32`, `STAT64`, `SIGACTION32`, `SELECT`, `POLL`, `CLONE`, `DUP3`). Bu tablo JIT motorunun ABI sözleşmesidir; birim testleriyle korunur.
- `src/stack.c` — emüle edilmiş ilk yığın + auxv üretici (`AT_ENTRY`, `AT_HWCAP`(sahte armv7), `AT_PLATFORM="v7l"`, `AT_RANDOM`, `AT_PAGESZ=4096` …). JIT motoru için; qemu motoru kendi yığınını kurar.
- `src/engine_qemu.c` — v0.1 motoru: `qemu-arm [-L sysroot32] [-0 orig_argv0] target args…` argv kurulumu (saf fonksiyon, test edilir) + `execv`.
- `src/engine_jit.c` — gelecek Triango JIT için arayüz iskeleti (`probe() = -ENOSYS`).
- `src/main.c` — argv sözleşmesi çözümü, `--probe`, `--selftest`, loglama.

**Sysroot32:** motor, 32-bit bionic kütüphanelerine ihtiyaç duyar. Beklenen düzen:

```
/data/adb/triango/sysroot32/
├── system/bin/linker            (arm32 bionic linker)
├── system/lib/                  (libc.so, libm.so, libdl.so, liblog.so …)
└── vendor/lib/                  (gerekirse)
```

Kaynak: `armeabi-v7a` firmware/AOSP arm derlemesi (bionic Apache-2.0 — yeniden dağıtım uygundur). Motor yolu: `/data/adb/triango/engines/qemu-arm` (statik AArch64).

## 5. L2 — Motorlar / Engines

| Motor | v0.1 | Not |
|---|---|---|
| `qemu` | ✅ | Hızlı çözüm; qemu-arm user-mode (statik). Android bionic syscall yüzeyi qemu linux-user ile uyumludur. |
| `jit` | 🚧 | arm32→arm64 blok yeniden derleyici (FEX/box64 tarzı). `syscalls.c` tablosu + `stack.c` onun için hazırlandı. |

Motor arayüzü (`include/triango/engine.h`): `probe()` + `launch(spec)`. Yeni motor eklemek = yeni `.c` + tabloya bir satır.

## 6. L3 — KernelSU / BakaSU entegrasyonu

- Standart Magisk/KernelSU modül düzeni (`module.prop`, `post-fs-data.sh`, `service.sh`, `sepolicy.rule`, `system.prop`, `uninstall.sh`).
- **LKM jailbreak mode uyumu:** ksud late-load'ta `post-fs-data` geç çalışabilir → `service.sh` idempotent kontrol ile gerekli kurulumu tekrar dener (bayrak dosyası ile çift çalışmayı engeller).
- `.ko` yükleme: `insmod` (su altında). Vermagic GKI ile eşleşmiyorsa binfmt_misc yedeği kullanılır ve kullanıcı bilgilendirilir.
- `system.prop` içindeki `ro.product.cpu.abilist32` satırları **varsayılan olarak kapalıdır** (yalnızca yorum satırı) — PackageManager'ı kandırmak kurulumu mümkün kılar ama uygulama düzeyi çalışma (zygote32/ART) v0.3'e kadar desteklenmez.

## 7. Bilinen sınırlamalar / Known limitations (v0.1)

1. **Performans:** qemu user-mode çevirisi günlük kullanım için yavaştır — JIT motoru bu yüzden yol haritasında.
2. **Uygulama düzeyi:** APK içindeki arm32 native kod için ART/zygote32 entegrasyonu (libhoudini/ndk_translation benzeri) gerekir; v0.1 yalnızca **doğrudan exec** (shell, termux, oyun konsolu/native binary'ler, test araçları) hedefler.
3. **32-bit GPU/medya kütüphaneleri:** yalnızca CPU çevirisi yapılır; vendor arm32 HAL yoksa ilgili işlevler çalışmaz.
4. **vdso32 yok:** 32-bit vdso emüle edilir (qemu kendi vdso'sunu sağlar; JIT `stack.c` ile üretir).

## 8. Hata modları / Failure modes

| Belirti | Neden | Çözüm |
|---|---|---|
| `ENOEXEC` devam ediyor | `.ko` yüklenmedi | `su -c insmod …`, log: `dmesg \| grep triango` |
| `triango-bridge: engine not found` | qemu motoru yok | `tools/build-qemu-engine.sh`, motoru `/data/adb/triango/engines` altına kopyala |
| `linker not found` / dlopen hataları | sysroot32 eksik | `docs/bakasu-lkm-jailbreak.md` §sysroot |
| SELinux reddi | policy eksik | `module/sepolicy.rule` uygulandı mı? (`dmesg \| grep avc`) |
| Modül yüklendi ama exec değişmedi | `android_only=1` ve hedef statik Linux ELF | `android_only=0` yap veya hedefin `PT_INTERP`'ini kontrol et |
