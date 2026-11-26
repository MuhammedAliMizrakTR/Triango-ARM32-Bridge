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

- [ ] `tools/build-qemu-engine.sh` ile statik qemu-arm üretimi (CI artefaktı)
- [ ] sysroot32 paketleyici (armeabi-v7a bionic + linker çıkarımı/otomatik paketleme)
- [ ] `system.prop` ile `ro.product.cpu.abilist32` deneysel açma (PackageManager kurulumu)
- [ ] Bellek/performans: qemu `plugin`/TB cache ayarları, çoklu çekirdek
- [ ] logcat entegrasyonu (`liblog`) ve yapılandırılabilir log seviyesi

## v0.3 — Uygulama düzeyi (Houdini benzeri)

- [ ] zygote32 / `app_process32` yolu: arm32 ART + 32-bit framework kütüphaneleri
- [ ] ART "isa çevirici" köprüsü (ndk_translation/libhoudini benzeri arayüz)
- [ ] Binder/servis köprüsü: arm32 uygulama ↔ 64-bit system_server
- [ ] SurfaceFlinger/GPU: arm32 vendor lib'leri olmadan EGL/Vulkan çağrısı çevirisi

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
