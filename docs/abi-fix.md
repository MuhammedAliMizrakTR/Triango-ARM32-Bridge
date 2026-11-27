# `INSTALL_FAILED_NO_MATCHING_ABIS` — sistem düzeyinde ABI sorunu

## Belirti / Symptom

Arm32 (armeabi-v7a) bir APK kurulurken sistem reddeder:

```
Failure [INSTALL_FAILED_NO_MATCHING_ABIS: Failed to extract native libraries]
```

Veya `PackageManager` güncellemeleri aynı sebeple düşer. "Sistem içinde Failed
No Matching ABis olayı" budur.

## Neden / Root cause

Kurulumda `PackageManager` → `NativeLibraryHelper.copyMatchingAbis()` APK'nın
`lib/armeabi-v7a` içeriğini, cihazın **desteklediği ABI listesi** ile eşler.
Bu liste `Build.SUPPORTED_ABIS`'ten gelir ve **`ro.product.cpu.abilist`**
prop'larından türetilir. Galaxy S26 Ultra'da (ve tüm 64-bit-only Qualcomm
cihazlarda):

```
ro.product.cpu.abilist=arm64-v8a          # armeabi-v7a YOK
ro.product.cpu.abilist32=                 # boş
```

Eşleşme bulunamaz → `INSTALL_FAILED_NO_MATCHING_ABIS`.

## Çözüm katmanları / Fix layers

### 1. Tek kurulumluk geçici çözüm (araç)

```bash
tools/pm-install-arm32.sh oyun.apk
# → adb install --abi armeabi-v7a oyun.apk
```

`pm install --abi`, PM'nin ABI filtresini tek seferlik atlar. Sistem geneli
sorunu çözmez.

### 2. Sistem geneli çözüm: prop reklamı (Triango modülü, v0.2) ✅

Triango modülü, **post-fs-data** aşamasında (system_server başlamadan önce —
zamanlama kritiktir, `Build.SUPPORTED_ABIS` erken önbelleklenir) şu prop'ları
ayarlar:

```
ro.product.cpu.abilist=arm64-v8a,armeabi-v7a,armeabi
ro.product.cpu.abilist32=armeabi-v7a,armeabi
ro.product.cpu.abilist64=arm64-v8a
ro.vendor.product.cpu.abilist=arm64-v8a,armeabi-v7a,armeabi
ro.vendor.product.cpu.abilist32=armeabi-v7a,armeabi
```

- `resetprop` ile (KernelSU/Magisk); bulunamazsa `module/system.prop` devrede.
- **Kapatmak için:** `/data/adb/triango/config/disable_abilist32` dosyasını
  oluşturun.

Sonuç: arm32 APK'lar **kurulur** ve `lib/armeabi-v7a` kütüphaneleri
doğru ABI ile ayıklanır.

### 3. Çalıştırma (bu adım v0.3) — NativeBridge

Kurulumun çözülmesi **çalıştırma** anlamına gelmez: stok sistemde
`zygote32`/`app_process32` ve arm32 ART yoktur. Nihai çözüm Android'in resmi
ISA-çeviri kancasıdır (libhoudini/ndk_translation'ın da kullandığı):

```
ro.dalvik.vm.native.bridge=libtriango_native_bridge.so
```

`libtriango_native_bridge.so` = Android **NativeBridge** arayüzünü
uygulayan Triango kütüphanesi: ART, arm32 native yüklemelerini Triango
çeviri motoruna (qemu/JIT) devreder. Yol haritası v0.3
(`docs/roadmap.md`); GPU tarafı `docs/gpu-thunking.md` ile birlikte
"3D uygulama çalıştırma" bileşkesini tamamlar.

## Doğrulama / Verification

```bash
# Prop'lar
adb shell getprop ro.product.cpu.abilist        # arm64-v8a,armeabi-v7a,armeabi
adb shell getprop ro.product.cpu.abilist32      # armeabi-v7a,armeabi

# Kurulum testi
adb install ornek-arm32.apk                     # SUCCESS beklenir
adb shell pm list packages | grep ornek

# Hâlâ red alınıyorsa: PM önbelleği/geç kalma — reboot sonrası tekrar deneyin
# ve dmesg/logcat'te NativeLibraryHelper satırlarına bakın.
```

## SSS / FAQ

**S: 64-bit uygulamalar etkilenir mi?** Hayır; liste `arm64-v8a`'yı içerir,
sıralama onu öncelikli tutar.

**S: Play Store arm32 uygulamaları "uyumsuz" mu diyecek?** Play filtreleri de
aynı prop'lara bakar; reklam sonrası uyumlu görünür (cihaz sertifikası
düzeyinde ek etkenler olabilir).

**S: Kurulan arm32 uygulama şimdi çalışır mı?** Hayır — çalıştırma için
v0.3 (NativeBridge + zygote32/ART) gerekir; şu an yalnızca kurulum/akış
sorunu giderilir. (Konsol/native binary'ler `triango-bridge` ile zaten
çalışır — o ayrı yoldur.)
