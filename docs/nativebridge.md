# Triango NativeBridge — v1.0

Bu belge `libtriango_native_bridge.so`'yu ve KSU modül entegrasyonunu
tanımlar: **LateLoad uyumlu**, **soft reboot ile aktive olabilen** ve
**WebUI**'li arm32 çalıştırma köprüsü. Kod: `nativebridge/`, modül:
`module/`.

## 1. NativeBridge nedir?

Android'in resmi ISA-çeviri kancası (libhoudini / ndk_translation'ın da
kullandığı arayüz). ART, `ro.dalvik.vm.native.bridge` ile işaret edilen
kitaplığı yükler ve arm32 `.so` yüklemelerini ona devreder:

```
zygote / uygulama süreci (arm64 ART)
   │  System.loadLibrary("oyun")  ← lib/armeabi-v7a/liboyun.so
   ▼
libnativebridge (ART)  ──dlsym("NativeBridgeItf")──►  libtriango_native_bridge.so
   │                                                    │  isSupported(): ELF32+EM_ARM mı?
   │  getTrampoline()                                    │  loadLibrary(): motor kaydı
   ▼                                                    ▼
çevrilmiş kod çağırılabilir ◄──────────── Triango motoru (qemu/JIT eklentisi)
```

Arayüz doğrulaması AOSP kaynağından yapıldı (`native_bridge.cc`:
`dlsym("NativeBridgeItf")` → **veri sembolü**; sürümler 1–8, biz **v6**'yı
uygularız). Referans: AOSP `test/115-native-bridge/nativebridge.cc`.

## 2. Bileşenler

| Dosya | Ne yapar |
|---|---|
| `nativebridge/lib/nb_triango.c` | `NativeBridgeItf` (v6) — initialize/gating/load/getTrampoline/getAppEnv/… |
| `nativebridge/lib/nb_elf.c` | ELF32+EM_ARM sınıflandırıcı (isSupported kapısı — gerçek mantık) |
| `nativebridge/lib/nb_engine.c` | Motor eklentisi yükleyici (`dlopen`, LateLoad için **tembel yeniden deneme**) |
| `nativebridge/include/triango/nb_engine.h` | Motor eklentisi ABI'si (load_library/get_trampoline/unload) |
| `module/` | KSU modülü v1.0: props, linkerconfig, activate.sh, WebUI |
| `tools/build-nativebridge.sh` | NDK ile gerçek bionic `.so` üretimi |

**Dürüstlük notu:** arayüz, kapılama, motor bağlama ve yaşam döngüsü gerçek
ve host testlerinde doğrulanmıştır (43 kontrol); **çevirinin kendisi**
motor eklentisindedir (qemu köprüsü / Triango JIT — roadmap). Motor yokken
`loadLibrary()` temiz hata döndürür, zygote çökmez.

## 3. Motor eklentisi (engine plug-in)

`libtriango_nb_engine.so` şu sembolleri dışa açar:

```c
const triango_nb_engine_ops *triango_nb_engine_open(const char *private_dir);
void triango_nb_engine_close(void);
```

Arama sırası: `$TRIANGO_NB_ENGINE` → `/data/adb/triango/lib/…` → modül
dizini. **LateLoad uyumu:** arama her `loadLibrary()`'de yeniden denenir —
motor dosyası zygote başladıktan sonra sahnelenmiş olsa bile bir sonraki
yüklemede yakalanır.

## 4. Soft reboot ile aktivasyon (tam yeniden başlatma gerekmez)

`module/activate.sh` (WebUI'deki **"Aktive Et · Soft Reboot"** butonu):

1. `libtriango_native_bridge.so`'yu `/data/adb/triango/lib/` altına sahneler
   (modül zip'indeyse `customize.sh` kurulum anında da yapar);
2. `/linkerconfig/ld.config.txt` içine bu dizin için izin ekler
   (namespace `search.paths`/`permitted.paths`) — böylece zygote
   `/data/adb` altından `dlopen` yapabilir;
3. `resetprop` ile `ro.dalvik.vm.native.bridge` **mutlak yola** ve
   `ro.product.cpu.abilist*` ayarlar;
4. `setprop ctl.restart zygote` ile çerçeveyi yeniden başlatır (soft reboot).

Sonuç: **modül kur → WebUI → Aktive Et** — tam reboot yok. (Sistemless
`system/lib64` mount'u tam boot'ta da hazırdır; iki yol birbirini yedekler.)

Kapatma: `/data/adb/triango/config/disable_nativebridge` (abilist için:
`disable_abilist32`).

## 5. WebUI (KernelSU)

`module/webroot/index.html` — tek dosya, çevrimdışı, koyu tema:

- **Durum kartları:** NativeBridge prop, ABI prop, bridge kütüphanesi, gpud
- **İşlemler:** Aktive Et · Soft Reboot · gpud Başlat/Durdur · Tam Reboot
- **Günlük:** nativebridge.log + gpud.log canlı görüntüleme
- **TR/EN** dil değiştirici; `ksu.exec` köprüsü savunmacı (WebUI-X uyumlu)

## 6. Motor eklentisi — V1.1(a) (`libtriango_nb_engine.so`)

`nativebridge/engine/` içindeki yorumlayıcı motor, `nb_engine.h` ABI'sini
gerçekler: `load_library` (ELF32 EM_ARM `.so` yükleyici), `get_trampoline`
(host'tan çağrılabilir giriş noktası), `unload_library`, `get_error`.

- **Yorumlayıcı:** ARM + Thumb-2 tam sayı alt kümesi (DP, LDR/STR ailesi,
  LDM/STM, MUL/MLA/MLS/UMLAL, UDIV/SDIV, MOVW/MOVT, bitfield, B/BL/BLX,
  CBZ, TBB/TBH …). Her host iş parçacığına bir CPU — çok çekirdekli
  çağrılar güvenli.
- **libc shim'leri (native hız):** memcpy/memmove/memset/strlen/strcmp/
  strncmp/strcpy/strncpy/strcat/strchr/malloc/calloc/realloc/free/abort/
  puts/abs + `__aeabi_uidiv/idiv/memcpy*/memclr*/memset/memmove`. Bunlar
  doğrudan host libc'sinde koşar — "native hız" bu katta zaten var.
- **Trambolinler:** x86_64 (SysV) ve aarch64 (AAPCS64) için W^X stub'lar;
  bridge'in `get_trampoline` çağrıları gerçek fonksiyon işaretçisi alır.
- **Çok modüllü:** bağımlı semboller yüklenmiş modüllerden + shim
  tablosundan çözülür; çözülemeyen sembol hatası raporlanır.

```bash
make -C nativebridge engine        # libtriango_nb_engine.so
make -C nativebridge engine-test   # 30 test (blobs + trambolin [+ TRIANGO_TEST_SO])
```

### Dürüstlük / honesty (V1.1(a))

| İddia | Gerçek durum |
|---|---|
| "arm32 uygulama çalışır" | ✅ yorumlayıcı ile çalışır; 30 otomatik test |
| "native hız" | ⚠️ libc içe aktarımları native; uygulama kodu yorumlanır. **JIT native hızı sonraki sürüm.** |
| "tam CPU" | ✅ iş parçacığı başına CPU (çok çekirdekli yorum) |
| "tam GPU util" | ✅ mevcut gpud dişli yolu (v1.0) |
| VFP/NEON | ❌ bilinçli boşluk — trap + tanılama (V1.2) |
| SVC (syscall) | ❌ trap + tanılama (V1.2: shim/syscall köprüsü) |
| Sinyal çevirisi | 🗺️ henüz NULL (motorla birlikte) |

## 7. Bilinen sınırlar / Roadmap

| Konu | Durum |
|---|---|
| Arayüz (v6), gating, motor bağlama, yaşam döngüsü | ✅ gerçek + testli |
| arm32 kod **çalıştırma** | ✅ V1.1(a) yorumlayıcı (30 test) |
| arm32 kod **native hız** (JIT) | 🚧 sonraki adım (roadmap) |
| libc içe aktarımları | ✅ host shim'leri (native hız) |
| VFP/NEON + SVC | 🗺️ V1.2 |
| Sinyal çevirisi (getSignalHandler) | 🗺️ motorla birlikte (şimdilik NULL) |
| JNI trampoline kısa-yolu (shorty) | ⚠️ V1.1(a): shorty yok sayılır, 4×u32 marşalling |
| 32-bit uygulama süreci (zygote32, tam uyumluluk) | 🗺️ alternatif yol: arm32 `app_process32` + binfmt köprüsü |

## 8. Doğrulama

```bash
make -C nativebridge test          # host: 43 kontrol
make -C nativebridge engine-test   # motor: 30 test
adb shell getprop ro.dalvik.vm.native.bridge
adb shell ls -l /data/adb/triango/lib/
adb logcat -s triango              # veya /data/adb/triango/log/nativebridge.log
```
