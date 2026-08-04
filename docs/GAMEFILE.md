# The character inside a saved game

A save archive stores the character **twice**:

| Member | What it is | Who reads it |
|---|---|---|
| `hero` | packed `PkPlayerStruct`, 1266 bytes | the character-selection screen |
| `game` | full `PlayerStruct` inside the saved game | `LoadGame` → `LoadPlayer`, once you play |

`game` is present only while a game is in progress. Editing `hero` alone shows
on the selection screen and is then overwritten from `game` the moment play
starts — visible, but not effective. `src/gamefile.cpp` keeps the two in step.

## Format

`game` is codec-encoded with the same password as `hero`. Decoded, it opens
with a four-byte magic:

| Magic | Game | Dungeon levels |
|---|---|---|
| `RETL` | Diablo | 17 |
| `HELF` | Hellfire | 25 |
| `SHAR` | Diablo shareware | 17 |
| `SHLF` | Hellfire shareware | 25 |

Then `LoadGame`'s own prologue — 39 more bytes, big-endian — and eight bytes per
dungeon level. The player record follows, at `43 + levels*8`.

The record is the raw 32-bit `PlayerStruct` as it sat in memory. The original
does `memcpy(&plr[i], tbuff, ...)` (devilution `Source/loadsave.cpp`), and
DevilutionX's field-by-field reader, with all its `Skip(4)` calls for pointers
and alignment, describes that same image. **There is one format here, not two.**
Its fields are little-endian, even though the prologue around it is big-endian.

Nothing is variable-length, so edits are byte patches and the rest of the file —
monsters, objects, dungeon, ground items — never has to be parsed.

## The field map

Offsets relative to `_pName`, which is 320 into the record.

| Field | Offset | Type | `hero` equivalent |
|---|---:|---|---|
| spell levels, ids 0–36 | −127 | 37 bytes | `pSplLvl` |
| spell levels, ids 37–46 | −90 | 10 bytes | `pSplLvl2` |
| `_pMemSpells` | −56 | u64 | `pMemSpells` |
| `_pName` | 0 | 32 bytes | `pName` |
| `_pClass` | +32 | i8 | `pClass` |
| `_pBaseStr` | +40 | i32 | `pBaseStr` |
| `_pBaseMag` | +48 | i32 | `pBaseMag` |
| `_pBaseDex` | +56 | i32 | `pBaseDex` |
| `_pBaseVit` | +64 | i32 | `pBaseVit` |
| `_pStatPts` | +68 | i32 | `pStatPts` |
| `_pHPBase` | +80 | i32 | `pHPBase` |
| `_pMaxHPBase` | +84 | i32 | `pMaxHPBase` |
| `_pManaBase` | +100 | i32 | `pManaBase` |
| `_pMaxManaBase` | +104 | i32 | `pMaxManaBase` |
| `_pLevel` | +120 | i8 | `pLevel` |
| `_pExperience` | +124 | i32 | `pExperience` |
| `_pGold` | +140 | i32 | `pGold` |

The two spell runs are contiguous: `game` holds ids 0–46 in one array, which
`hero` splits across `pSplLvl[37]` and `pSplLvl2[10]`.

Derived values — `_pStrength`, `_pMaxHP`, `_pHitPoints`, `_pDamageMod` — are
**not** written. `LoadPlayer` ends by calling `CalcPlrItemVals`, which rebuilds
them from the base fields plus equipment. Writing them would only risk
disagreeing with what the game computes.

## Why nothing here trusts an offset

Every offset derived by *reading the game's source* was wrong:

- the prologue, by 12 bytes — the DevilutionX revision read was three months
  from the build being targeted;
- `_pName`, by 16 — `PLR_NAME_LEN` is 32, not 16;
- the gap between the spell array and `_pMemSpells`, by 5.

Every offset derived by *matching against `hero`* was right, and consistent
across files. Patching 200 KB of game state at a wrong offset is unrecoverable,
so the module works the second way:

1. Find the record by searching for the character's name.
2. Compare **every** field in the table against `hero`.
3. Require several agreements where at least one side is non-zero — zero on both
   proves nothing, since a wrong offset landing in padding also reads zero.
4. Require exactly one position to pass. Two, or none, is a refusal.

The game writes both members together, so on a healthy save they agree. That
makes step 2 a free validity proof, per file and per game version: a layout that
has shifted fails the check instead of corrupting the save.

Two details that check produced, which reading the source would not have:

- The stats are `BYTE` in `hero` but `int32` in `game`. Read as signed, a
  dexterity of 250 becomes −6 — and would have been written as −6.
- The game does not clear the tail of `_pName` when a character is renamed, so a
  save can hold `rogue\0jh` where the packed copy holds `rogue\0\0\0`. The name
  is compared to its terminator, not across the whole field.

## Verified against

Two real DevilutionX 1.2.1 Hellfire saves — a Rogue at level 25 and a Monk at
level 3 — with all seventeen fields agreeing in both.

`RETL` (Diablo) is exercised by a synthetic saved game with the shorter
17-level prologue, which moves the record and everything after it. Nothing
notices, and that is the point: **neither anchor is computed from the level
count** — the name and `InvGrid` are both searched for — so the only thing the
flavour changes is which magic is accepted and a label in `inspect`.

It has still **not** been run against a real Diablo saved game, and the honest
statement is that this proves the code path rather than the byte layout. The
record is the same `PlayerStruct` in both games, and a layout that had shifted
would fail verification rather than corrupt anything.

## The inventory

`game` stores full `ItemStruct`s where the packed copy stores 17-byte seeds.
This matters for gold: `ValidatePlayer` recomputes `_pGold` from the stacks
every tick, so changing the packed copy alone is undone on the first frame.

Anchored on `InvGrid` — 40 bytes byte-identical in both copies, with `_pNumInv`
as the `int32` immediately before it.

| | |
|---|---|
| `ItemStruct` in file | **372 bytes** (one 8-byte pointer becomes 4) |
| `_iSeed` | +0 — equals the packed item's `iSeed` |
| `_itype` | +8 (`ITYPE_GOLD` = 11) |
| `_iCurs` | +192 |
| `_ivalue` | +196 |
| `InvBody[7]` | `_pNumInv − 47×372` |
| `InvList[40]` | `_pNumInv − 40×372` |
| `SpdList[8]` | after `InvGrid` |

**Items are matched by seed, not by position.** `hero_set_gold` compacts the
list — removing gold stacks shifts everything after them down — so an item that
merely moved has to be relocated rather than rebuilt. The packed `iSeed`
equalling the saved game's `_iSeed` makes that exact, and gives a per-item
validity proof independent of where anything sits.

**New gold stacks are cloned, never synthesised.** A pile the game itself wrote
is copied and only three fields changed: `_iSeed`, `_ivalue`, and `_iCurs`
(large at ≥2500, small at ≤1000, medium between — `Source/inv.cpp`). Every
other byte is the game's own. A character carrying no gold has nothing to clone
from, and that is refused rather than guessed at.

It refuses, rather than guessing, when:

- any item in the on-disk character is not found in the saved game by seed —
  the two copies have diverged and writing would corrupt one of them;
- an item is neither matched nor gold, so there is nothing to build it from;
- new gold is needed and the character carries none.

A 90,000-gold edit on a real save moved 1,317 bytes, all between `_pGold` and
the end of `InvGrid`, and re-running it changed nothing.

## Still not covered

Equipment (`InvBody`) and the belt (`SpdList`) are located but not written —
butcher does not edit those anyway. Non-gold items cannot be created, only
moved, because everything but gold is generated from a seed by the game's own
item generator.
