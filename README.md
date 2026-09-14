# Equip Enchantment Fix NG

A CommonLibSSE-NG rewrite of [SlavicPotato's Equip Enchantment Fix](https://www.nexusmods.com/skyrimspecialedition/mods/42839) (MIT).

Fixes engine bugs where worn item enchantments don't apply on equip, get wrongly dispelled while still worn, or go missing after loading a save. Works on the player and NPCs.

## Compatibility

- Skyrim SE 1.5.97
- Skyrim AE 1.6.x (1.6.318 - 1.6.1170)
- Skyrim AE 1.7.99 / 1.7.104 (requires SKSE64 2.3.0+)
- Skyrim VR is **not** supported

## Requirements

- [SKSE64](https://skse.silverlock.org/) matching your game version
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) with data for your game version
- [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG) at build time (see `COMMONLIB_SSE_FOLDER` in CMakeLists.txt)
- vcpkg (see `vcpkg.json`)

## Build

```powershell
$env:VCPKG_ROOT="E:\vcpkg"
cmake -B build -S . -G 'Visual Studio 18 2026' -A x64 `
  -DCMAKE_TOOLCHAIN_FILE='E:/vcpkg/scripts/buildsystems/vcpkg.cmake' `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
  -DVCPKG_HOST_TRIPLET=x64-windows-static-md
cmake --build build --config Release
```

Use an ASCII-only path for the project checkout: MSVC misreads a non-ASCII PCH path (MSB8084 / C4828).

## Configuration

`Data/SKSE/Plugins/EquipEnchantmentFix.ini`:

```ini
[EEF]
OnEquip=true
OnActorLoad=true
RedirectDispelWornItemEnchantsVisitor=true
RecalcPlayerInventoryWeightOnLoad=false
```

## Notes

The original plugin redirected the engine's dispel visitor with code hooks. This port achieves the same coverage through public CommonLibSSE-NG APIs instead: a removed effect re-queues the actor for a check one frame later, so anything wrongly dispelled is restored. The end state is identical; the effect may blink for a single frame.

## Credits

- SlavicPotato - original mod and MIT source
- alandtse / CharmedBaryon and the CommonLibSSE-NG contributors

## License

MIT, inherited from the original (see LICENSE).
