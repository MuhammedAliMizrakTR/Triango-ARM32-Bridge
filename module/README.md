# module/ — KernelSU / BakaSU module package

Magisk/KernelSU-compatible module layout. `tools/package-module.sh` zips this
directory together with the built payloads into
`dist/triango-arm32-bridge-ksu.zip`.

| File | Rol |
|---|---|
| `module.prop` | modül kimliği |
| `post-fs-data.sh` | kurulum: dosya sahneleme, `triango_bridge.ko` yükleme, binfmt_misc yedeği |
| `service.sh` | geç yükleme (jailbreak mode) güvenlik ağı + durum kaydı |
| `uninstall.sh` | binfmt kaydı temizliği (`echo -1 > entry`), `/data/adb/triango` silme |
| `sepolicy.rule` | minimal SELinux izinleri |
| `system.prop` | deneysel `abilist32` prop'ları (varsayılan **kapalı**) |
| `triango-bridge-lkm` | argv sözleşmesi sarmalayıcısı (`--conv=lkm`) |

Paketleme sırasında zip köküne eklenen payload'lar:

- `triango-bridge` — AArch64 köprü ikilisi (`bridge/` çıktısı)
- `triango_bridge.ko` — LKM (`kernel/triango-bridge-lkm/` çıktısı, CI artefaktı)

Kurulum ve doğrulama: `docs/bakasu-lkm-jailbreak.md`.
