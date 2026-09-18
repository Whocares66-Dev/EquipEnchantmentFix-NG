# Equip Enchantment Fix NG

A CommonLibSSE-NG rewrite of [SlavicPotato's Equip Enchantment Fix](https://www.nexusmods.com/skyrimspecialedition/mods/42839) (MIT). This port is GPL-3.0-or-later - see [License](#license).

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

The original plugin (1.3.5) installed raw code hooks to stop a duplicate enchantment
apply - and a bad dispel - before either happened. This port does the same, at the
same three call sites: the engine's call to `Actor::UpdateArmorAbility` is skipped
when the actor already carries that item's enchantment, and the two calls to
`Actor::DispelWornItemEnchantments` on the trade path (the container transfer and
the model rebuild it triggers) go to a replacement that dispels only effects whose
source item is no longer worn. Nothing worn is ever dispelled, so nothing has to be
re-applied and the numbers are right while the menu is still open.
`RedirectDispelWornItemEnchantsVisitor` controls all three and defaults to true.

The call sites are found at launch, not carried as offsets: given the caller's and
the callee's Address Library ids, the site is the one `call` inside the caller that
lands on the callee. A caller with no such call, or two, is refused and logged.

The ability check itself is the exact test the original used: walk the actor's
active effects and match the `(source, spell)` pair, where `source` is the item and
`spell` is its enchantment.

## Credits

- SlavicPotato - original mod and MIT source
- alandtse / CharmedBaryon and the CommonLibSSE-NG contributors

## License

[GPL-3.0-or-later](COPYING.txt), with the exceptions in [EXCEPTIONS.md](EXCEPTIONS.md).

That is not a choice: this plugin statically links
[CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG), which is
GPL-3.0-or-later, and a plugin that links it forms a combined work with it.

The original [Equip Enchantment Fix](https://www.nexusmods.com/skyrimspecialedition/mods/42839)
by SlavicPotato is MIT; that text is kept at
[LICENSES/LICENSE-MIT.txt](LICENSES/LICENSE-MIT.txt) for attribution. MIT is
GPL-compatible, so combining it here is fine, but the combined work is GPL.
