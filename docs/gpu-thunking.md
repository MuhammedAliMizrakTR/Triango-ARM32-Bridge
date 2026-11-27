# GPU Thunking — OpenGL ES & Vulkan (dişli / normal)

Bu belge Triango'nun GPU katmanını tanımlar: **OpenGL ES** ve **Vulkan** için
iki çalışma biçimi — **normal** (doğrudan) ve **dişli** (geared/thunked) — ve
bunların mimarisi, protokolü ve yol haritası. Uygulama kodu: `gpu/`.

## 1. İki çalışma biçimi / Two modes

| | **Normal (doğrudan)** | **Dişli (geared / thunked)** |
|---|---|---|
| Fikir | Misafir (arm32) uygulama, **arm32 yerel sürücü** kitaplıklarını kullanır | Misafir çağrıları **köprüden** (thunk) geçer; yürütme **arm64 sürücüde** yapılır |
| Ne gerekir | sysroot32'te çalışan arm32 Adreno blob'ları + kgsl ioctl uyumu | arm32 shim kütüphaneleri + `triango-gpud` + arm64 sürücü |
| GLES | arm32 `libGLESv2`/`libEGL` → arm32 blob → `/dev/kgsl-3d0` | arm32 shim → kanal → gpud → arm64 `libGLESv2_adreno` |
| Vulkan | arm32 Vulkan loader + arm32 ICD (doğrudan geçiş) | dispatch-tablo marshalling → gpud → arm64 ICD |
| Durum | **Geçiş/uyumluluk modu**: yalnızca eski blob'larla ve sınırlı cihazlarda çalışır (S26 Ultra'da arm32 blob **yok**) | **Varsayılan hedef yol** — S26 Ultra için doğru mimari |

Her iki API için de seçim motor/thunk yapılandırmasıyla yapılır
(`TRIANGO_GPU_MODE=normal|geared`, varsayılan `geared`).

## 2. Dişli modun mimarisi / Geared-mode architecture

```
arm32 uygulama (qemu TCG veya Triango JIT içinde)
   │  eglSwapBuffers(), glDrawArrays(), vkCreateInstance() ...
   ▼
arm32 SHIM kütüphaneleri (sysroot32'te libEGL.so / libGLESv2.so / vulkan.so)
   │  gen_thunks.py ile üretilen çağrı kodu
   ▼
paylaşımlı bellek KANALI (gpu_proto.h: iki SPSC byte-ring, slot+payload mesajı)
   ▼
triango-gpud (arm64 daemon)
   │  üretilen host dispatch → gerçek sembol (dlsym)
   ▼
arm64 sürücü: libEGL_adreno/libGLESv2_adreno  veya  Zink + Mesa Turnip (Vulkan)
```

Seçilen üretici modeli (FEX/box64/ndk_translation ailesi): **API başına
tanımlı marshalling** — `gpu/thunks/*.json` fonksiyon imzalarını tipleriyle
listeler, `gpu/gen_thunks.py` hem arm32 shim'ini hem arm64 dispatch'ini üretir.

### 2.1 Kanal protokolü (`gpu/include/triango/gpu_proto.h`)

- İki SPSC byte-ring (istek/yanıt), serbest koşan sayaçlarla, kayıt
  sarmalama (wrap) destekli; `[u32 len][mesaj][8'e tampon]` çerçevesi.
- Mesaj: `[hdr(16)][nslots × u64][payload]`.
- **Slot kodlaması:** skaler/handle → 1×u64; string/buffer → `off<<32|len`
  (payload içinde kopya). Misafir işaretçileri asla kanaldan **geçmez**.
- **Jeton-eşleme tablosu** (tur 2): driver handle'ları 64-bit'tir; misafir
  (arm32) yalnızca 32-bit tutabilir. gpud tarafında `tg_token_map` her
  driver handle'ı yoğun bir **32-bit id**'ye sarar (id 0 = NULL); argümanlar
  id→handle, dönüşler handle→id olarak çevrilir. Böylece misafir kırpma
  kayıpsızdır. `handle_array_out` için `elem: u32` (GL isimleri — jetonsuz)
  ve `elem: u64` (EGL/Vulkan — jetonlu) ayrımı vardır.
- Yanıt, istek payload'unu **yerinde** geri verir (buffer çıkışları host'ta
  doldurulur, shim geri kopyalar); string dönüşleri payload sonuna eklenir,
  shim TLS arabelleğine kopyalar.

### 2.2 Sözleşme testleri (host üzerinde, CI'da)

`make -C gpu test` — 4000+ kontrol:

- ring sarmalama / doluluk / çerçeve bütünlüğü (`test_proto.c`)
- **uçtan uca roundtrip**: üretilmiş EGL/GLES2/Vulkan stub'ları → kanal →
  sahte arm64 sürücü → sonuç (`test_roundtrip.c`): float slotları,
  buffer-in baytları, buffer-out kopya geri dönüşü, dinamik string dönüşü,
  handle-array eleman genişliği dönüşümü, `ENOTSUP`/`ENOSYM`/`EAGAIN` yolları
- **32↔64 struct dönüşümü** kanıtı (`test_struct_conv.c`): VkApplicationInfo
  arm32 (28 bayt) ↔ LP64 (48 bayt) düzeni + konuk string kopyalama
- ASan/UBSan ile temiz.

## 3. OpenGL ES yolu / OpenGL ES path

- **Shim:** sysroot32'te `libEGL.so` + `libGLESv2.so`, üretilen stub'lardan
  oluşur (v0.1 JSON kapsamı: EGL 1.4 çekirdeği + GLES2 çekirdeği ~34 giriş).
- **Eksik/kasalı girişler** (JSON'a bilinçli alınmadı): `glShaderSource`
  (dizi-işaretçi marshalling), `glReadPixels` (büyük buffer geçişi — payload
  limiti), `eglChooseConfig` (handle dizisi genişlik dönüşümü). Sıradaki
  üretim turlarında eklenecek; protokol zaten taşıyabiliyor.
- **Sürücü:** varsayılan arm64 Qualcomm blob'ları (`libEGL_adreno`,
  `libGLESv2_adreno`). Alternatif açık kaynak rota: GLES→**Zink**→Vulkan.

## 4. Vulkan yolu / Vulkan path

Vulkan'ın zorluğu **struct düzeni**: `VkInstanceCreateInfo` vb. tüm
`pNext` zincirli yapılar 32↔64 farklı yerleşir. Yaklaşım:

1. Handle-bazlı ve skaler girişler **doğrudan** marshal edilir
   (`vkGetInstanceProcAddr`, `vkDestroy*`, `vkEnumerateInstanceVersion`,
   `vkEnumeratePhysicalDevices` — çalışan örnekler).
2. Struct-bazlı girişler **`struct_conv: true`** ile işaretlenir ve
   `TGPU_ST_ENOTSUP` döner — dönüştürücü kitaplığı (`gpu/struct_conv/`)
   tamamlanana kadar bilinçli ve güvenli bir engel.
3. Dönüştürücü kanıtı hazır: `tg_vk_conv_app_info_32to64()` (VkApplicationInfo)
   — misafir adres uzayını `tg_guest_map_fn` ile okuyarak string'leri kopyalar.
   Yol haritası: `pNext` zincir yürüteci + JSON'dan otomatik struct
   tanımları (sType tabanlı).
4. **ICD seçimi:** gpud, arm64 `vulkan.adreno.so`'yu (blob) veya **Mesa
   Turnip**'i yükler. Turnip, Adreno 840'ı (SM8850) tanır — açık kaynak
   Vulkan yolu cihazda canlıdır.

## 5. Bilinen sınırlar (tur 2 sonrası) / Known limitations

| Konu | Durum | Plan |
|---|---|---|
| Handle genişliği (arm32 4 bayt vs host 8 bayt) | ✅ **jeton-eşleme tablosu** (tg_runtime: 64-bit driver handle ↔ 32-bit misafir id) | — |
| `glShaderSource` (string dizisi) | ✅ `string_array` marshaller (`tg_pack_strings`) | — |
| `glReadPixels`/`glTexImage2D`/`eglChooseConfig` | ✅ `len_expr` + `count_from` (skaler/buffer) | — |
| GL isim dizileri (glGen* u32) vs EGL/Vulkan jetonları (u64) | ✅ `elem: u32/u64` ayrımı | — |
| `VkInstanceCreateInfo` & pNext zinciri | `ENOTSUP` | struct_conv üreteci (v0.3) |
| ANativeWindow / gralloc / BufferQueue | yok | EGL window surface'lar için gerekli; NativeBridge/ART ile birlikte |
| Periyodik/async olaylar (vkQueueSubmit completion) | sync kanal | genişletilmiş mesaj kuyruğu + callback jetonu |
| Performans (roundtrip gecikmesi) | kabul edilebilir değil günlük için | JIT + yerinde komut tamponu (command buffer) biriktirme |

Tur 2 kapsamı: EGL 27 giriş (configs/choose/pbuffer/query/swapinterval/…),
GLES2 ~70 giriş (shader/uniform/doku/FBO/pixels/…), Vulkan 11 giriş
(device queue/wait idle/…) — `make -C gpu test` 4098 kontrol, ASan/UBSan
temiz.

## 6. Sınıflandırma: "dişli" nerede devreye giriyor?

| Katman | Normal | Dişli |
|---|---|---|
| Yükleyici (LKM/binfmt) | aynı | aynı |
| Motor (qemu/JIT) | aynı | aynı |
| GL/Vulkan kitaplıkları | arm32 yerel blob | **arm32 shim → gpud** |
| Yürütme (GPU) | arm32 blob → kgsl ioctl | **arm64 sürücü** (blob veya Turnip) |

Yani "dişli", **yalnızca grafik yığını** için farklıdır; CPU çevirisi
(§ architecture.md) her iki modda da aynıdır.
