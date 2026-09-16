# Changelog

All notable changes to the NG port. 1.3.7 - 1.3.18 were published on Nexus;
1.3.19 was an internal build that has been folded into 1.3.20.

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
