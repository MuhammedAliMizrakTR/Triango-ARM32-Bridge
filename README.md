# Triango — ARM32 Bridge

> **TR —** Qualcomm/Oryon tabanlı yeni nesil yonga setlerinde (Snapdragon 8 Elite, **8 Elite Gen 5 / SM8850** …) tamamen kaldırılan **Arm32 / AArch32** çalıştırma desteğini geri kazandırmayı amaçlayan açık kaynak köprü projesi. Dağıtım ve yönetim hedefi: **KernelSU — BakaSU LKM Jailbreak Mode** (kilitli bootloader, ksud late-load). İlk hedef cihaz: **Samsung Galaxy S26 Ultra (m3q)**.
>
> **EN —** An open-source bridge that brings back **Arm32 (AArch32) execution** on Qualcomm SoCs that dropped it (Oryon: Snapdragon 8 Elite / 8 Elite Gen 5). Designed to be deployed and managed under **KernelSU — BakaSU in LKM Jailbreak Mode** (locked bootloader, ksud late-load). Primary target device: **Samsung Galaxy S26 Ultra (m3q)**.

---

## Neden? / Why?

Snapdragon 8 Elite (SM8750) ve 8 Elite Gen 5 (SM8850) ile Qualcomm, Oryon CPU çekirdeklerinde AArch32 EL0 desteğini tamamen kaldırdı. Sonuç:

- `armeabi-v7a` APK'lar **kurulamaz ve çalışmaz** (`abilist32` boş, zygote32 yok).
- 32-bit ELF'ler `execve`'de **ENOEXEC** alır — donanım AArch32 talimat setini yürütemez.
- Samsung Galaxy S26 Ultra kutudan **"Apps: 64-bit only"** çıkar (bkz. `docs/device-s26-ultra.md`).

Triango bu boşluğu **kullanıcı alanı çeviri/emülasyon köprüsü + ince bir çekirdek LKM** ile kapatır: 32-bit ARM ELF'ler yakalanır, AArch64 köprüsüne devredilir ve bir motor (v0.1: `qemu-arm` user-mode; gelecek: Triango JIT) üzerinden çalıştırılır.

Qualcomm's Oryon cores (Snapdragon 8 Elite / 8 Elite Gen 5) have **no AArch32 EL0 support at all**. Triango closes the gap with a userspace translation/emulation bridge plus a thin kernel LKM: Arm32 ELFs are intercepted and executed through an engine (v0.1: `qemu-arm` user-mode; future: Triango JIT).

## Nasıl çalışır? / How it works

```
 execve("oyun32", ...)                         (arm32 ELF, EM_ARM)
        │
        ▼
 ┌─────────────────────────────┐
 │ triango_bridge.ko  (LKM)    │  yüksek öncelikli linux_binfmt
 │ ELF32+EM_ARM yakalama       │  (insert_binfmt — binfmt_elf'den önce)
 └──────────────┬──────────────┘
                │ argv: [bridge, /path/oyun32, orig_argv0, args…]
                ▼
 ┌─────────────────────────────┐
 │ triango-bridge  (AArch64)   │  ELF doğrulama, sysroot32 hazırlama,
 │                             │  motor seçimi
 └──────────────┬──────────────┘
                ▼
 ┌─────────────────────────────┐
 │ engine: qemu-arm (v0.1)     │  arm32 user-mode emülasyon
 │ engine: triango-jit (v0.2+) │  arm32→arm64 yeniden derleyici
 └─────────────────────────────┘
```

Ayrıntılı mimari: [`docs/architecture.md`](docs/architecture.md)

## Hedef cihaz / Target device

| | Samsung Galaxy S26 Ultra |
|---|---|
| Model | SM-S948B / SM-S948U (kod adı **m3q**) |
| SoC | Qualcomm Snapdragon 8 Elite Gen 5 for Galaxy (**SM8850-AC**), Oryon 2+6 |
| CPU | 64-bit **yalnızca** — AArch32 EL0 **yok** |
| Android | 16 (API 36), One UI 8.5 |
| Kernel | `6.12.30-android16-5-…-4k` (GKI android16-6.12, 4 KB sayfa) |
| Durum | Arm32 uygulamalar: ❌ (stok) → Triango ile kademeli ✅ |

Diğer 64-bit-only Qualcomm cihazlar (SM8750/SM8850 tabanlı OnePlus, Xiaomi, Pixel-vari tüm cihazlar) da aynı yoldan faydalanır; bkz. `docs/device-s26-ultra.md`.

## Depo düzeni / Repository layout

```
kernel/triango-bridge-lkm/   Çekirdek LKM — binfmt yakalama (GPL-2.0)
bridge/                      Kullanıcı alanı köprüsü (AArch64, taşınabilir C)
  src/                       ELF32 yükleyici, syscall tablosu, motor arayüzü
  tests/                     Host üzerinde çalışan birim testleri
gpu/                         GLES/Vulkan dişli (thunk) katmanı + gpud daemon
module/                      KernelSU / BakaSU modül paketi (Magisk uyumlu düzen)
tools/                       Derleme, paketleme, sembol denetimi yardımcıları
docs/                        Mimari, cihaz profili, BakaSU dağıtım rehberi
```

## Derleme / Build

```bash
# 1) Kullanıcı alanı köprüsü + birim testleri (herhangi bir Linux host)
make -C bridge test

# 2) GPU dişli katmanı testleri (protocol + roundtrip + struct_conv)
make -C gpu test

# 3) Köprüyü hedefe (aarch64) derle
make -C bridge CROSS_COMPILE=aarch64-linux-gnu- all

# 4) LKM — GKI kaynak ağacı gerekir (CI otomatik derler)
./kernel/triango-bridge-lkm/build.sh /path/to/kernel-common-android16-6.12

# 5) KernelSU modül zip'i üret
./tools/package-module.sh
# → dist/triango-arm32-bridge-ksu.zip
```

## Dağıtım — KernelSU / BakaSU LKM Jailbreak Mode

1. Cihazda **BakaSU** (veya KernelSU) kurulu olsun — *LKM mode + jailbreak mode* (kilitli BL, ksud late-load) Triango'nun birincil hedef ortamıdır.
2. `dist/triango-arm32-bridge-ksu.zip` dosyasını **BakaSU Manager → Modules → Install from storage** ile kurun.
3. Motor + 32-bit sysroot'u hazırlayın (bir kez): `tools/build-qemu-engine.sh` çıktısını ve `armeabi-v7a` sysroot'u `/data/adb/triango/{engines,sysroot32}` altına kopyalayın (bkz. `docs/bakasu-lkm-jailbreak.md`).
4. Yeniden başlatın; `su -c triango-bridge --probe` ile durumu doğrulayın.

> LKM doğrudan da yüklenebilir: `su -c insmod /data/adb/triango/triango_bridge.ko`.
> Modül, `.ko` yüklenemezse **binfmt_misc** yedeğiyle (kullanıcı alanı) çalışır.

Ayrıntılar: [`docs/bakasu-lkm-jailbreak.md`](docs/bakasu-lkm-jailbreak.md)

## GPU desteği / GPU support (v0.2)

OpenGL ES ve Vulkan için **iki** çalışma biçimi:

- **Normal:** misafir (arm32) doğrudan yerel sürücü kitaplıklarını kullanır
  (sysroot32'te arm32 blob varsa).
- **Dişli (geared):** arm32 shim → paylaşımlı bellek kanalı → `triango-gpud`
  → arm64 sürücü (Adreno blob **veya** Mesa Turnip Vulkan).

Mimari, protokol, struct-dönüşüm ve bilinen sınırlar:
[`docs/gpu-thunking.md`](docs/gpu-thunking.md).

## `INSTALL_FAILED_NO_MATCHING_ABIS` (v0.2)

Sistem geneli ABI prop'ları (abilist32) modülle otomatik açılır; tek
kurulumluk geçici çözüm: `tools/pm-install-arm32.sh`. Ayrıntılar:
[`docs/abi-fix.md`](docs/abi-fix.md).

## NativeBridge modülü v1.1(a) (WebUI · soft reboot · motor)

`libtriango_native_bridge.so` (Android NativeBridge v6) + **yorumlayıcı motor
eklentisi** `libtriango_nb_engine.so` + KSU modülü: **LateLoad uyumlu**,
**soft reboot ile aktive olur** (tam yeniden başlatma gerekmez), **KernelSU
WebUI** (durum kartları, tek dokunuşla aktivasyon, günlük görüntüleyici).
Ayrıntılar: [`docs/nativebridge.md`](docs/nativebridge.md).

**Dürüstlük notu (V1.1(a)):** motor, arm32 ARM/Thumb-2 komutlarını
**yorumlayıcı** ile çalıştırır — libc içe aktarımları (memcpy, malloc,
strlen, __aeabi_*) **host üzerinde native hızda** koşar; uygulama kodu
çeviri öncesi hızdadır. **Gerçek native hız (JIT)** bir sonraki adımdır.
VFP/NEON ve SVC çağruları bilinçli boşluklardır (tanılama ile raporlanır).

## Durum / Status (v1.1.0-a)

| Bileşen | Durum |
|---|---|
| `triango_bridge.ko` (binfmt yakalama, GKI 6.12 uyumlu) | ✅ CI'da derlenir |
| `triango-bridge` (ELF32 yükleyici, argv sözleşmesi, motor arayüzü) | ✅ + host birim testleri |
| `qemu-arm` motor entegrasyonu | ✅ argv kurulumu hazır (motor binary'si harici) |
| GPU dişli katmanı (thunk üretici + protokol + jeton tablosu + gpud) | ✅ `make -C gpu test` (4098 kontrol) |
| GPU normal mod (doğrudan arm32 blob geçişi) | ✅ tasarım + sysroot32 yerleşimi hazır |
| `INSTALL_FAILED_NO_MATCHING_ABIS` çözümü (abilist32) | ✅ modülde (post-fs-data resetprop + system.prop) |
| NativeBridge (libtriango_native_bridge, v6 arayüz + WebUI + soft reboot) | ✅ v1.1(a) |
| **Motor eklentisi** (`libtriango_nb_engine.so`: ARM/Thumb-2 yorumlayıcı + ELF32 yükleyici + host trambolin + libc shim'leri) | ✅ V1.1(a) — 30 test |
| KernelSU / BakaSU modül paketi (WebUI'li) | ✅ v1.1.0-a |
| Triango JIT motoru (arm32→arm64, native hız) | 🚧 sonraki adım (`docs/roadmap.md`) |

## Güvenlik / Safety

- Bu proje **araştırma/kişisel cihaz** amaçlıdır; kilitli bootloader jailbreak araçlarıyla birlikte kullanım her zaman risk taşır (veri kaybı, brick, garanti).
- Kernel modülü yalnızca yetkili (`root`) yükleme yoluyla çalışır; SELinux politikası `module/sepolicy.rule` ile sınırlı tutulur.
- Yalnızca **kendi cihazınızda** kullanın.

## Lisans / License

Kullanıcı alanı kodu: **MIT** (`LICENSE`). Çekirdek modülü: **GPL-2.0-only** (SPDX başlıkları geçerlidir).

---

### Katkı / Contributing

Fikir, hata raporu ve PR'lar bekleniyor. Özellikle: arm32 sysroot paketleme, JIT motor tasarımı ve ART/zygote32 entegrasyon denemeleri.
