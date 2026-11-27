# Dağıtım Ortamı — KernelSU / BakaSU LKM Jailbreak Mode

Triango, kilitli bootloader'lu Samsung cihazlarda (S26 Ultra) çalışmak zorunda. Bu ortamın
kurallarını ve Triango'nun buna uyumunu anlatır.

## BakaSU nedir?

**BakaSU** (eski adıyla ReSukiSU), KernelSU tabanlı bir root çözümüdür. KernelSU'nun iki
kurulum modu vardır:

| Mod | Açıklama | Triango için |
|---|---|---|
| **GKI mode** | `kernelsu.img` kernel'e gömülür / boot'a yazılır | Genellikle unlocked bootloader gerektirir |
| **LKM mode** | `kernelsu.ko` **yüklenebilir çekirdek modülü** olarak geç yüklenir | ✅ birincil hedef |

**"Jailbreak mode"** = LKM mode'un kilitli bootloader senaryosu: cihaz açıldıktan *sonra*
bir kullanıcı alanı exploit/araç (ör. Shizuku + `finit_module`) `kernelsu.ko`'yu yükler;
**ksud** geç (late) yükleme yolunda modül sistemini ayağa kaldırır (`post-fs-data`,
`service`, kernel event'leri). BakaSU bu yolu resmi olarak destekler ve menüde
**"[LKM] [jailbreak mode]"** rozetini gösterir.

Sonuç olarak Triango:

1. **Bir KernelSU/BakaSU modülü** olarak paketlenir (`module/`) — Manager üzerinden kurulur.
2. `triango_bridge.ko`'yu root altında `insmod` ile yükler (kendisi de bir LKM'dir ama
   kernel'e kalıcı olarak gömülmesi gerekmez).
3. Her açılışta (late-load dahil) **idempotent** kurulum yapar.

## Zamanlama / Timing — neden `service.sh` idempotent?

Normal açılışta sıra: `post-fs-data` → `service`. Jailbreak late-load'ta ise `post-fs-data`
**çoktan geçmiş olabilir**; ksud yine de modüllerin `post-fs-data.sh`'ini geç çalıştırır,
ancak her modül buna güvenmemelidir. Triango bu yüzden:

- kritik kurulumu (dosya sahneleme + `.ko` yükleme + binfmt yedeği) **tek bir
  `triango_setup()` fonksiyonunda** toplar,
- hem `post-fs-data.sh` hem `service.sh` bu fonksiyonu çağırır,
- `/data/adb/triango/.setup_done` bayrak dosyası çift çalışmayı engeller (boot başına temizlenir).

## Kurulum adımları / Install

```bash
# 1) Modül zip'i (host'ta)
./tools/package-module.sh
adb push dist/triango-arm32-bridge-ksu.zip /sdcard/

# 2) BakaSU Manager → Modules → Install from storage → yeniden başlat

# 3) Motor + sysroot (bir kez)
adb push qemu-arm /sdcard/triango-engine
adb shell su -c 'mkdir -p /data/adb/triango/engines /data/adb/triango/sysroot32'
adb shell su -c 'cp /sdcard/triango-engine /data/adb/triango/engines/qemu-arm && chmod 755 /data/adb/triango/engines/qemu-arm'
# sysroot32 içeriğini de /data/adb/triango/sysroot32 altına kopyalayın (bkz. docs/architecture.md §4)

# 4) Doğrulama
adb shell su -c '/data/adb/triango/triango-bridge --probe'
adb shell su -c 'dmesg | grep triango'
```

## El ile LKM yükleme / Manual LKM load

Modül yerine doğrudan (ör. exploit sonrası tek atış):

```bash
su -c 'insmod /data/adb/triango/triango_bridge.ko bridge_path=/data/adb/triango/triango-bridge'
su -c 'echo 1 > /sys/module/triango_bridge/parameters/enabled'
```

`insmod` verimiği (vermagic) GKI ile eşleşmeli: **S26 Ultra için
`6.12.30-android16-5-…-4k`**. Vermagic/sembol uyuşmazlığında `.ko` yüklenmez; modül
otomatik olarak **binfmt_misc yedeğine** düşer.

## Sembol denetimi / Symbol check

LKM yalnızca export edilmiş sembolleri kullanır. Cihazda:

```bash
su -c 'grep -E " (copy_string_kernel|bprm_change_interp|open_exec|__register_binfmt|unregister_binfmt)$" /proc/kallsyms'
```

Beşi de görünmeli (GKI ABI listesinde oldukları için modüllerden erişilebilir olmalı).
`tools/check-ksyms.sh` bu kontrolü otomatik yapar (`adb` üzerinden).

## binfmt_misc yedeği / Fallback

`.ko` yüklenemezse `post-fs-data.sh` binfmt_misc kaydı yapar:

```
:triango-arm32:M::\x7fELF\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x28\x00:
  \xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xff\xff:
  /data/adb/triango/triango-bridge-lkm:P
```

- Sihir: ELF32 + LSB + `e_machine=EM_ARM(0x0028)` (offset 18); `e_type` maskesiz
  (ET_EXEC ve ET_DYN/PIE eşleşir — köprü doğrular).
- **`P` (preserve-argv0)** bayrağı: kernel dokümanına göre argv
  `[interpreter, binary_path, orig_argv0, orig_args…]` olur — yani **LKM ile birebir
  aynı sözleşme**. `triango-bridge-lkm` sarmalayıcısı `--conv=lkm` geçer
  (bkz. `docs/architecture.md` §2.1).
- Kayıt idempotenttir: `triango-arm32` zaten varsa tekrar yazılmaz.

## SELinux

- `module/sepolicy.rule` minimal izinleri verir (modül domain'lerinin `/data/adb/triango`
  altına exec/read, binfmt_misc mount).
- `enforcing` **kapatılmaz**; Triango enforcing altında çalışacak şekilde tasarlanır.
- Redler için: `dmesg | grep avc | grep triango`.

## Güvenlik notları / Security notes

- `kernelsu.ko` + exploit tabanlı geç yükleme her zaman risklidir; Triango bu zincire
  yalnızca *sonradan eklenen* bir katmandır.
- Triango, `su` yetkisi olmayan süreçlerden **etkilenmez** (binfmt kaydı root gerektirir).
- Modül, kaldırıldığında (`uninstall.sh`) binfmt kaydını ve `/data/adb/triango` içeriğini temizler.

## SSS / FAQ

**S: Magisk'te çalışır mı?** Evet — düzen Magisk/KernelSU uyumludur; Triango'nun *öncelikli*
hedefi BakaSU LKM jailbreak mode olsa da normal Magisk kurulumunda da aynı scriptler çalışır.

**S: Bootloader kilidi açıksa?** GKI mode / Magisk de kullanılabilir; `.ko` yine `insmod`
ile (veya Magisk'in `post-fs-data`'sında) yüklenir.

**S: Neden her açılışta yeniden yükleme?** LKM jailbreak mode kalıcı değildir; ksud
late-load her boot'ta tekrarlanır. Triango'nun kurulumu buna göre idempotenttir.
