# Hedef Cihaz — Samsung Galaxy S26 Ultra (m3q)

Bu sayfa Triango'nun birincil hedef cihazının donanım/yazılım profilini ve Arm32 kaybının
teknik sonuçlarını belgeler. (Firmware doğrulamaları Kasım 2026 itibarıyla.)

## Donanım / Hardware

| Özellik | Değer |
|---|---|
| Model(ler) | SM-S948B (global), SM-S948U (US), SM-S948W/SM-S948N (varsayılan) |
| Kod adı | **m3q** |
| SoC | Qualcomm **Snapdragon 8 Elite Gen 5 for Galaxy** — **SM8850-AC** |
| CPU | Qualcomm Oryon — 2× Prime (4.61 GHz) + 6× Performance (3.63 GHz) |
| GPU | Adreno 840 |
| RAM | 12/16 GB LPDDR5X |
| Mimari | **64-bit yalnızca (AArch64)** |

### Arm32 neden yok?

Qualcomm, Oryon mimarisiyle (SM8750 "Snapdragon 8 Elite" ile başlayarak) **AArch32 EL0
desteğini donanımdan kaldırdı**. Bu bir yazılım kısıtı değil; CPU hiçbir koşulda 32-bit
ARM talimatlarını yürütemez:

- `system_supports_32bit_el0()` → `false`
- Çekirdek `CONFIG_COMPAT` derlense bile `execve` üzerindeki ELF32 `ENOEXEC` döner
- `/system/lib` (32-bit) ve `zygote32` / `app_process32` firmware'den çıkarılmıştır
- `ro.product.cpu.abilist32` boş; `ro.zygote=zygote64`

## Yazılım / Software (stok)

| Özellik | Değer |
|---|---|
| Android | 16 (API 36 / SDK 36) |
| One UI | 8.5 |
| Kernel | `6.12.30-android16-5-pd30ff70-abogki…-4k` (GKI **android16-6.12**, 4 KB sayfa) |
| Derleme örneği | `BP4A.251205.006.S948BXXS4AZG5` (kasım 2026) |
| Uygulama ABI'leri | **64-bit only** (firmware etiketi: "Apps: 64-bit only") |

Notlar:
- GKI **android16-6.12** dalı → LKM'ler bu ağaca karşı derlenmelidir (`build.sh` bu dalı alır).
- `4k` son eki sayfa boyutunun 4096 olduğunu doğrular — 32-bit misafir (qemu/JIT) için de 4096 kullanılır (`stack.c` `AT_PAGESZ=4096`).
- Samsung kilidi bootloader; kalıcı root **yok**. Triango bu yüzden **LKM + ksud late-load
  (BakaSU LKM Jailbreak Mode)** üzerine kuruludur: her açılışta modül yeniden yüklenir.

## Cihazda doğrulama komutları / On-device verification

```bash
# Kernel ve sayfa boyutu
uname -r                    # 6.12.30-android16-5-...-4k beklenir
getconf PAGESIZE            # 4096

# Arm32 izleri
getprop ro.product.cpu.abilist        # arm64-v8a beklenir (armeabi-v7a YOK)
getprop ro.product.cpu.abilist32      # boş beklenir
getprop ro.zygote                      # zygote64

# Triango durumu (kurulum sonrası)
su -c 'cat /sys/module/triango_bridge/parameters/enabled'
su -c '/data/adb/triango/triango-bridge --probe'
```

## Triango'nun cihazda geri kazandırdıkları

| Seviye | Ne çalışır | v0.1 | v0.3 (hedef) |
|---|---|---|---|
| Shell/CLI | `arm32` ELF binary'ler (termux paketleri, test araçları, oyun konsolları) | ✅ | ✅ |
| 32-bit kitaplıklar | `dlopen("libfoo.so" /* arm32 */)` köprü süreci içinde | ✅ (sysroot32 ile) | ✅ |
| APK kurulumu | `abilist32` prop kandırmacası ile Paket Yöneticisi | 🗺️ (deneysel, `system.prop`) | ✅ |
| APK çalıştırma | zygote32 + ART arm32 + çeviri | ❌ | ✅ |

## Diğer etkilenen cihazlar / Other affected devices

Aynı mimari kısıt (AArch32'siz Oryon/64-bit-only) şunlarda da vardır — Triango'nun yolu
cihazdan bağımsızdır: Snapdragon 8 Elite (SM8750) / 8 Elite Gen 5 (SM8850) tabanlı tüm
cihazlar (OnePlus 13/14/15/16, Xiaomi 15/16/17 serisi, Galaxy S25/S26 serisi, iQOO, Realme
GT serisi …). Cihaz profilleri `docs/` altında ileride ayrı sayfalara taşınacaktır.

## Kaynakça / References

- Qualcomm Oryon / Windows-on-Arm AArch32 kaldırma duyurusu (Build 2023, XDA/Windows Central özetleri).
- Galaxy S26 Ultra firmware bilgileri (SM-S948B/U, `6.12.30-android16-5-…-4k`), sammfrew/firmware günlükleri, kasım 2026.
- KernelSU / BakaSU LKM jailbreak-mode literatürü (ksud late-load), bkz. `docs/bakasu-lkm-jailbreak.md`.
