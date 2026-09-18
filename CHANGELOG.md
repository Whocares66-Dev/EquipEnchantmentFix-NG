# Changelog

All notable changes to the NG port. 1.3.7 - 1.3.21 have been released;
1.3.19 was an internal build that was folded into 1.3.20.

## 1.3.22 (wip)

**The dispel redirect is back.** Giving a follower any item in the trade menu
dropped every enchantment on the armour they were wearing (Nordic Souls #183).

- The container menu's transfer routine calls `Actor::DispelWornItemEnchantments`
  on the NPC and then asks for a model update; only the update re-applies, and it
  only runs when an equipment change flagged it. A potion flags nothing. When
  equipment did change, the update runs at once and a rebuild helper inside it
  dispels everything again, then runs the actor's magic update with a zero
  step, so the old effects finish on the spot while the re-equipped copies
  only start at the next update, after the menu. Measured on 1.6.1170.
- Both calls, the same two the original plugin's
  `RedirectDispelWornItemEnchantsVisitor` (1.3.5) replaced and 1.3.6 dropped,
  now go to a replacement that keeps an effect when its source item and its
  enchantment match a worn instance and dispels the rest. The enchantment is
  picked as the engine picks it, the record's first, then the instance's.
  Nothing worn is ever dispelled, so nothing has to be re-applied and the
  numbers are right while the menu is open. Matching on the item alone would
  keep an effect while any copy of that item is worn, the case 1.3.4 fixed in
  the re-apply path. The redirect is only installed when the
  `UpdateArmorAbility` hook is, since the armour trade's re-equip would stack a
  copy without it.
- Call sites are found at launch from the caller's and the callee's ids: the
  one `call` inside the caller that lands on the callee, the caller bounded by
  the next function the Address Library maps. No offset is carried for any
  version; a caller with no such call, or two, is refused and logged. The
  rebuild helper was renumbered at 1.6.629 and a lookup of an absent id is
  fatal, so on AE its site is found without its id, as the one caller of the
  dispel that is neither the transfer routine nor the dispel-and-recast one.
- A dispelled effect no longer counts as "present". The engine's re-equip of a
  worn item unequips first, which flags the effect; the flagged copy stays
  listed until the actor's next update, and counting it blocked the re-apply.
  Both the de-dup hook and the re-check skip flagged copies now, as the
  engine's own dispel-and-recast does.
- Removed the re-check on `TESActiveEffectApplyRemoveEvent` (the 1.3.9 -
  1.3.21 "RedirectDispel"). The engine sends that event only for an effect
  with a unique id, and assigns one only when the magic effect form's runtime
  flag bit 22 is set, which no MGEF has on disk and which by all appearances
  marks scripted effects. A plain enchantment effect never raises it, so the
  re-check only ever ran after spell effects ended, for nothing.
- `SKSE::Init` no longer takes over logging (it replaced the plugin's logger
  with one fixed at info). `LogLevel=debug` in the ini shows every step of the
  redirect and the re-check.

## 1.3.21

**The engine's duplicate apply is now blocked before it happens** - the same
mechanism the original plugin used.

- Hooks the engine's call to `Actor::UpdateArmorAbility` and skips it when the
  actor already carries that item's enchantment, matched on the same
  `(source, spell)` pair the repair path uses. 1.3.20 could only restore an
  enchantment after a bad dispel; this stops the duplicate apply at source, so
  nothing blinks.
- `RedirectDispelWornItemEnchantsVisitor` now defaults to **true**, matching the
  original plugin. The post-hoc recheck stays as the fallback if the hook cannot
  be installed.
- The hook patches the call site only after verifying the byte there really is a
  `call` (`E8`/`E9`). If a future runtime moves it, the plugin logs and falls back
  to the recheck rather than patching blind.
- Hooking the call site rather than the function keeps our own repair calls
  working, since they reach the function directly.
- Verified in game on SE 1.5.97: 42 unequip/equip cycles of an enchanted item
  leave exactly one effect; a real unequip still removes it and re-equipping
  restores it; load drain 0 ms; no errors in any log.

- **License corrected to GPL-3.0-or-later.** This plugin statically links
  CommonLibSSE-NG, which is GPL-3.0-or-later and states that a linking plugin
  forms a combined work with it and must be GPL as well. The MIT text it
  inherited from the original plugin is kept for attribution only (see
  LICENSES/). Thanks to ChrysopoeiaAlchemy for pointing this out.

## 1.3.20

**Fixed: enchantments could stack on repeated equips** (the duplicate-enchantment
report that came in after 1.3.18).

The "is this enchantment already on the actor" check was `Actor::HasMagicEffect`.
That call **faults on every runtime tested, SE 1.5.97 included** - so it never
worked reliably: the failure path reported "missing", every equip looked like it
needed re-applying, and effects accumulated.

It is replaced with the test the original plugin used: walk the actor's active
effects and match the `(source, spell)` pair, where `source` is the item and
`spell` is its enchantment. `MagicTarget::GetActiveEffectList` is reached through
`AsMagicTarget()`, which supplies the correct `MagicTarget` offset (0x98 on SE,
0xA0 on AE); a bare `Actor*` would read the wrong vtable.

- Equipping no longer rescans the whole inventory: the equip event re-checks only
  the item that was actually equipped. This removes the stutter when switching
  weapons or armour.
- Removed the re-apply cooldown, the fault counter and the tri-state ability
  result - all of them existed only to work around the broken query above.

Verified in game on SE 1.5.97: 25 unequip/equip cycles of an enchanted item leave
exactly one effect each time; no faults; no spurious re-applies.

**On the loading freeze:** the load-time actor re-check is measured at **0 ms**
for ~450 actors. It is not the source of the multi-second load freeze reported
against 1.3.18.

## 1.3.18
- Removed the post-load actor sweep (it could apply the same item twice in one
  frame) and added a short re-apply cooldown. **Superseded by 1.3.20** - the
  cooldown is gone and the underlying check is now exact.

## 1.3.17
- Restored `TESObjectLoadedEvent` and batched the load-time re-checks into a
  single task, so streaming references no longer queue thousands of tasks.

## 1.3.16
- Replaced the per-reference load-time task with an actor-list sweep.

## 1.3.15
- Removed the temporary diagnostic logging.

## 1.3.14
- Swapped the active-effect walk for `Actor::HasMagicEffect` to stop the crash in
  `GetActiveEffectList` / `VisitEffects`. **Reverted in 1.3.20** - that call turned
  out to fault as well, and silently disabled the check.

## 1.3.13
- Used `MagicTarget::VisitEffects`.

## 1.3.11
- Added SEH (structured exception handling) around the fault-prone engine calls.

## 1.3.10
- Removed the form-table lookup from the equip handler (it crashed while the
  engine's form-table lock was held).

## 1.3.9
- Skip enchantments that are not constant-effect: a weapon enchantment moved onto
  armour by a "no enchantment restriction" mod drives the engine down a branch it
  does not expect and crashes.
- `RedirectDispelWornItemEnchantsVisitor` defaults to off.

## 1.3.8
- Fixed a crash in the deferred task.

## 1.3.7
- First CommonLibSSE-NG build.
