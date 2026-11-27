# Yol Haritası / Roadmap

## v0.1 — Temel köprü (bu sürüm) ✅

- [x] `triango_bridge.ko`: binfmt yakalama (GKI android16-6.12 uyumlu, yalnızca export semboller)
- [x] `triango-bridge`: ELF32 ARM ayrıştırıcı, argv sözleşmeleri (lkm / manual), `--probe`, `--selftest`
- [x] Motor arayüzü + `qemu-arm` motoru (argv kurulumu; motor binary'si harici)
- [x] Arm32 EABI → AArch64 syscall eşleme tablosu (JIT sözleşmesi, testlerle korumalı)
- [x] Emüle yığın/auxv üretici (JIT için)
- [x] KernelSU/BakaSU modül paketi + binfmt_misc yedeği + idempotent kurulum
- [x] Host birim testleri + CI (test + LKM derleme)

## v0.2 — Kullanılabilir günlük deneyim

- [x] **GPU dişli katmanı iskeleti** (GLES + Vulkan): kanal protokolü, thunk
  üreteci (`gpu/gen_thunks.py`), `triango-gpud` daemon iskeleti, 4000+ host
  testi — `docs/gpu-thunking.md`
- [x] **`INSTALL_FAILED_NO_MATCHING_ABIS` çözümü**: `abilist32` prop'ları
  (post-fs-data `resetprop` + `system.prop`) + `tools/pm-install-arm32.sh`
  — `docs/abi-fix.md`
- [ ] GPU turu 2: `glShaderSource`/`glReadPixels`/`eglChooseConfig` thunk'ları,
  gpud handle jeton-eşleme tablosu, ANativeWindow/gralloc köprüsü
- [ ] `tools/build-qemu-engine.sh` ile statik qemu-arm üretimi (CI artefaktı)
- [ ] sysroot32 paketleyici (armeabi-v7a bionic + linker çıkarımı/otomatik paketleme)
- [ ] Bellek/performans: qemu `plugin`/TB cache ayarları, çoklu çekirdek
- [ ] logcat entegrasyonu (`liblog`) ve yapılandırılabilir log seviyesi

## v0.3 — Uygulama düzeyi (Houdini benzeri)

- [ ] zygote32 / `app_process32` yolu: arm32 ART + 32-bit framework kütüphaneleri
- [ ] ART "isa çevirici" köprüsü (`ro.dalvik.vm.native.bridge=libtriango_native_bridge.so`)
- [ ] Binder/servis köprüsü: arm32 uygulama ↔ 64-bit system_server
- [ ] SurfaceFlinger/GPU: EGL/Vulkan **dişli** tamamlama (struct_conv üreteci,
  pNext zinciri; `docs/gpu-thunking.md` §4-5) — arm32 vendor lib'leri gerekmez

## v0.4 — Triango JIT motoru

- [ ] arm32→arm64 blok yeniden derleyici (decode → IR → AArch64 emit)
- [ ] `syscalls.c` tablosuna dayalı syscall trampolin (pointer-32, timespec32, stat64 fixup'ları)
- [ ] `stack.c` ile misafir proses yığını/auxv/vdso32 emülasyonu
- [ ] Self-modifying code / icache senkronizasyonu, sinyal çevirisi

## v1.0

- [ ] Seçilmiş popüler arm32 APK'ların uçtan uca çalıştığı doğrulanmış liste
- [ ] Cihaz profilleri: S26 Ultra (m3q) + SM8750/SM8850 ailesi
- [ ] Güvenlik denetimi + kararlılık (stress test, 24 saat oyun testi)

---

Katkı alanları: JIT tasarım dokümanı (`docs/` altında `jit-design.md` önerilir),
sysroot paketleme, ART entegrasyon denemeleri, cihaz test raporları.
