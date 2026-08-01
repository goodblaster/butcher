# butcher — Design Notes

Why butcher is built the way it is.

This began as a subproject inside the devilution tree and was written as a plan
before any code existed; it has been updated as each part landed, so it doubles
as a record of what was tried and what the measurements showed. Paths below
refer to the devilution submodule under `third_party/devilution`.

## Scope

**In scope.** Read a save file, print the character's identity and scalar attributes, change one or more of them, write the file back so the retail game and Devilution both load it without complaint.

Editable fields (all live in `PkPlayerStruct`, `structs.h:1462`):

| Field | Pack member | Type | Notes |
| --- | --- | --- | --- |
| Name | `pName[32]` | char[] | Renaming does **not** rename the containing `.sv` file; see "Name is a filename" below |
| Class | `pClass` | char | `PC_WARRIOR`/`PC_ROGUE`/`PC_SORCERER` — changing this is legal but leaves stats un-rerolled |
| Level | `pLevel` | char | |
| Experience | `pExperience` | int | Not cross-checked against `pLevel` by the game |
| Base Str/Mag/Dex/Vit | `pBaseStr`/`pBaseMag`/`pBaseDex`/`pBaseVit` | BYTE | Max 255 by type; the game also caps per class |
| Unspent stat points | `pStatPts` | BYTE | |
| HP / Mana | `pHPBase`, `pMaxHPBase`, `pManaBase`, `pMaxManaBase` | int | Fixed-point: the low 6 bits are fractional, so 1 HP = 64 |
| Gold | `pGold` | int | **Derived — see "Gold is a lie" below** |
| Spell levels | `pSplLvl[37]` | char[] | |
| Known spells | `pMemSpells` | uint64 | Bitmask, spell `n` = bit `n-1` |
| Dungeon level | `plrlevel` | BYTE | |
| Diablo kill level | `pDiabloKillLevel` | DWORD | Hero rank shown in the UI |

**Out of scope, deliberately.** Items. `PkItemStruct` stores an RNG seed plus a create-info word, and `UnPackItem` regenerates the item by replaying `RecreateItem`/`RecreateEar` against the game's item tables. There is no field holding "+3 fire damage" to edit — you would have to search seed space for an item with the properties you want, which is a different and much larger project. The editor will *display* item slots (base index and durability/charges) read-only.

Also out of scope: a GUI, multiplayer `.drv` hero files, and editing the `game`/`perml*`/`perms*` level files inside the archive.

**Hellfire `.hsv` saves are supported** — see the Hellfire section below. They were originally deferred here on the assumption that they would need separate handling; they do not.

## The hard constraint

`Source/` is a byte-accuracy reconstruction (see `CLAUDE.md`). **The editor must not require a single edit to any existing file under `Source/`, `3rdParty/`, or the root headers.** Adding a `#include`, an `#ifdef`, or an `extern` to `codec.cpp` to make it reusable would risk the binary diff. The editor consumes those files as-is or not at all.

That rules out linking `Source/*.cpp` directly — every one of them starts with `#include "all.h"`, which drags in `windows.h`, DirectDraw, DirectSound, and 150 module headers full of game globals. The plan below reuses the three files that actually matter by compiling them behind a shim header, unmodified.

## How a save is actually stored

Writing (`pfile_write_hero`, `Source/pfile.cpp:301`) runs four stages. Reading is the exact inverse.

```
PlayerStruct (in-memory, ~unbounded)
  │  PackPlayer()                        Source/pack.cpp:42
  ▼
PkPlayerStruct                            1266 bytes, #pragma pack(1)
  │  codec_encode(buf, 1266, 1288, pw)   Source/codec.cpp:99
  ▼
X-SHA-1 keystream XOR + 8-byte trailer    1288 bytes
  │  mpqapi_write_file("hero", ...)      Source/mpqapi.cpp:527
  ▼
PKWare-imploded 4096-byte sectors, MPQ block+hash entries
  ▼
single_%d.sv   (%d = 0..9, sitting next to Diablo.exe)
```

Details the editor depends on:

- **Container.** `single_0.sv` … `single_9.sv` are MPQ archives. The character occupies one archive; the slot number is positional, not derived from the name. A single-player archive holds `hero`, `game`, `perml00`–`perml16`, `perms00`–`perms16` and possibly `templ*`/`temps*`.
- **Fixed MPQ layout.** Devilution hardcodes it (`ParseMPQHeader`, `Source/mpqapi.cpp:339`): header at 0 (104 bytes, `headersize` 32, `sectorsizeid` 3 → 4096-byte sectors), block table at **104**, hash table at **32872**, both 2048 entries × 16 bytes, file data from **0x10068** (65640). Tables are encrypted with `Hash("(block table)", 3)` / `Hash("(hash table)", 3)`.
- **Sector layout.** `mpqapi_write_file_contents` (`Source/mpqapi.cpp:444`) writes a DWORD sector-offset table of `ceil(len/4096)+1` entries, then each 4096-byte sector PKWare-imploded. Block flags are `0x80000100` (exists + implode), and **the per-file MPQ encryption is not used** — flag `0x00010000` is absent, so no sector decryption is needed. That is a large simplification.
- **Passwords** (`Source/pfile.cpp:11`): single-player `"xrgyrkj1"`, multiplayer `"szqnlsk1"`; spawn builds use `"adslhfb1"`/`"lshbkfg1"`. Multiplayer saves may instead be keyed on `GetComputerName`, which is one reason they're out of scope.
- **Size.** `codec_get_encoded_len(1266)` → 1266 rounded up to 1280 plus an 8-byte `CodecSignature` = **1288 bytes** on disk before compression. `pfile_read_hero` accepts the file only if the decoded length equals `sizeof(PkPlayerStruct)` exactly.

The 1266 figure is hand-derived from the struct definition. **Task zero of Phase 1 is to assert it**, not trust it.

## Three landmines

These are the reasons this is a design document and not a one-afternoon script.

### 1. The hash is not SHA-1

`Source/sha.cpp` is X-SHA-1, and its header says so. It diverges from real SHA-1 in three ways at once:

- Message schedule omits the rotate: `W[i] = W[i-16] ^ W[i-14] ^ W[i-8] ^ W[i-3]` with no `rotl1` (`Source/sha.cpp:45`).
- `SHA1Input` never appends padding or a length field; it processes whole 64-byte blocks and drops any remainder. `SHA1Calculate` always feeds exactly 64 bytes.
- `SHA1Result` copies the five state words out as native little-endian DWORDs with no byte-order fixup.

No crypto library will reproduce this. `Source/sha.cpp` must be compiled into the editor verbatim.

There is also a live compiler hazard documented in that file: MSVC global optimizations turn `SHA1CircularShift(5, A)` into `ror edx, 0x1b`, which changes the digest and produces saves incompatible with vanilla. `sha.cpp` guards it with `#pragma optimize("g", off)` for `_MSC_VER >= 1930`. **The editor must verify its own build doesn't hit the same class of bug** — a round-trip test against a real save is the only honest check.

### 2. The key derivation depends on MSVC's `rand()`

`codec_init_key` (`Source/codec.cpp:18`) seeds `srand(0x7058)` and fills a 136-byte key buffer with `rand()` truncated to `char`. The keystream therefore depends on the *C runtime's* PRNG, not on anything in the repo.

MSVC and MinGW both use the msvcrt LCG:

```c
seed = seed * 214013 + 2531011;
return (seed >> 16) & 0x7fff;
```

glibc and Apple libc use entirely different generators. A natively-compiled macOS or Linux editor calling the system `rand()` will derive a different key, silently fail every checksum, and report every save as corrupt.

**Mitigation:** the shim provides its own `srand`/`rand` implementing the msvcrt LCG exactly, so the editor is byte-identical on every host. This is the single highest-risk item; validate it first (Phase 1) before building anything on top.

### 3. Devilution will happily destroy a save it doesn't recognise

`ParseMPQHeader` validates the layout, and on *any* mismatch it seeks to 0, calls `SetEndOfFile`, and starts a fresh empty archive — the original save is gone. That behavior is correct to preserve in the game (it's what the original did) but is unacceptable in an editor.

**The editor must never truncate.** It validates, and on mismatch it refuses and exits non-zero. Plus: always write to a temp file and rename over the original only on success, and require an explicit `--backup`-suppressing flag rather than defaulting to no backup.

## Two more traps in the data

**Gold is a lie.** `pGold` is a cached total, not the source of truth. `CalculateGold` (`Source/inv.cpp:2965`) recomputes it by summing `_ivalue` over every `ITYPE_GOLD` stack in `InvList` and `SpdList`, and the game calls it on essentially every inventory interaction. Setting `pGold = 999999` displays correctly until the player picks up a coin, at which point it snaps back to the real inventory total.

Editing gold properly means editing gold *stacks*: for each `PkItemStruct` in `InvList`/`SpdList` with `idx == IDI_GOLD`, `wValue` holds the pile amount. The game caps a pile at `GOLD_MAX_LIMIT` (5000, `defs.h:129`) and the field is a `WORD`. So `--gold N` should distribute `N` across existing gold stacks (and optionally free `InvGrid` slots), then set `pGold` to match. The editor should implement that and treat a bare `pGold` write as a `--force`-only escape hatch.

**HP and mana are fixed-point.** The low 6 bits are fractional; `UnPackPlayer` enforces `(pHPBase & 0xFFFFFFC0) >= 64` unless `killok`. The CLI should accept and display whole hit points and do the `<< 6` internally, with a `--raw` flag for the underlying value.

**Name is a filename, sort of.** `pfile_get_save_num_from_name` maps a name to a slot by linear search over the names read out of all ten archives; the `.sv` filename is just the slot index. So renaming inside `hero` is sufficient and the file keeps its name — but two characters must not end up sharing a name, or `pfile_get_save_num_from_name` will return the first match and the game will write one character's data over the other's slot. The editor must scan all ten slots and reject a rename that collides.

## Proposed layout

```

  Makefile              standalone; not referenced by root Makefile or MakefileVC
  README.md
  main.cpp              CLI parsing, dispatch
  shim.h                Win32 typedefs, msvcrt rand, no windows.h
  savefile.cpp/.h       MPQ open/read/write for the fixed devilution layout
  hero.cpp/.h           PkPlayerStruct <-> editable view; validation
  format.cpp/.h         human-readable rendering
  tests/
    roundtrip.cpp       load -> save -> byte-compare
    fixtures/           a real single_0.sv (see "Test data")
```

Reused verbatim, compiled from their existing paths, never edited:

- `Source/sha.cpp` — X-SHA-1
- `Source/codec.cpp` — `codec_encode` / `codec_decode` / `codec_get_encoded_len`
- `Source/encrypt.cpp` — `Hash`, `Encrypt`, `Decrypt`, `InitHash`, and the PKWare wrappers
- `3rdParty/PKWare/explode.cpp`, `implode.cpp` — sector compression

`shim.h` is the whole trick. Those four files need `BYTE`/`DWORD`/`WORD`/`BOOL`, `app_fatal`, and `rand`/`srand`; they do not need Windows.

**Neutralizing `all.h`.** Every `Source/*.cpp` opens with `#include "all.h"`. An earlier draft of this plan proposed shadowing it with a minimal `all.h` on the include path — *that does not work*: a quoted include resolves relative to the directory of the file containing the directive, so `Source/all.h` always wins over any `-I`/`-iquote` directory. The working approach is to trip its include guard. `shim.h` starts with `#define __ALL_H__`, is force-included via `-include`, and the `#include "all.h"` in `codec.cpp`/`sha.cpp` then expands to nothing. No shadow file, no drift risk, no edit to `Source/`.

The shim then supplies the Win32 types the root headers need and includes the *real* `defs.h`, `enums.h`, and `structs.h`, so struct layouts come from the authoritative source rather than a copy. Only four Win32 types turn out to be needed (`HWND`, `HANDLE`, `LPDIRECTSOUNDBUFFER`, `WAVEFORMATEX`), and all appear in structs the editor never touches; `PkPlayerStruct` contains no pointers and sits inside `#pragma pack(1)`, so host pointer width is irrelevant to it.

## CLI

```
butcher list <path-to-diablo-dir>
    Scan single_0.sv .. single_9.sv, print slot, name, class, level.

butcher show <save.sv>
    Full character dump: identity, stats, HP/mana, gold breakdown by
    stack, spell levels, known spells, equipment slots (read-only).

butcher set <save.sv> [--name S] [--level N] [--str N] [--mag N]
                         [--dex N] [--vit N] [--statpts N] [--exp N]
                         [--hp N] [--maxhp N] [--mana N] [--maxmana N]
                         [--gold N] [--spell NAME=LVL] ...
    Apply edits. Default: write single_N.sv.bak first, validate, then
    replace atomically. --dry-run prints the diff and exits.

butcher dump  <save.sv> -o hero.bin      # decoded PkPlayerStruct
butcher patch <save.sv> -i hero.bin      # re-encode a raw struct
```

`dump`/`patch` exist so the format work is verifiable independently of the field-editing work, and so anything the CLI doesn't expose is still reachable with a hex editor.

Every `set` runs the same validation the game would tolerate — stat caps by class, level 1–50, gold ≤ stack capacity, name length < 32 and no path separators — and refuses out-of-range values unless `--force`. `--force` is what makes this a *character editor* rather than a save fixer; it should be available but never the default.

## Build

A standalone `Makefile`, invoked explicitly, not wired into the root `Makefile` or `MakefileVC` — those two produce the accuracy-critical artifacts and must not grow new targets or new object files.

```bash
make -C tools/butcher           # host-native: clang/gcc on macOS, Linux, MinGW
make -C tools/butcher test
```

Host-native is the point: the editor should run on the machine where the saves live, without Wine and without `Storm.dll`. Nothing it links needs Windows once `shim.h` is in place.

CI: add a job to `.github/workflows/build_mac.yml` (or a new workflow) that builds the tool and runs the round-trip test. Do **not** add it to `.circleci/config.yml` — those jobs exist to measure binary accuracy and a tool build would only add noise to that signal.

## Phases

**Phase 1 — Prove the crypto (highest risk, do it first). — DONE, pending real-save verification.**

Implemented in ``; run with `make -C tools/butcher check`. 35 checks, all passing:

- Layout pinned at compile time. Measured, not assumed: `sizeof(PkItemStruct) == 19`, `sizeof(PkPlayerStruct) == 1266`, `codec_get_encoded_len(1266) == 1288`. The hand-derived 1266 in this document was correct; one offset in the original derivation was not (`pDiabloKillLevel` is at 1234, not 1238). All 24 field offsets the editor touches are now asserted.
- msvcrt LCG matches the published `srand(1)` sequence (41, 18467, 6334, …), the only external reference point available without a Windows host.
- X-SHA-1 characterization goldens recorded, plus a guard asserting the digest is *not* standard SHA-1 — so a future "fix" to `sha.cpp` fails loudly instead of silently producing unreadable saves.
- Full `codec_encode` → `codec_decode` round-trip recovers 1266 bytes byte-for-byte, is deterministic, and is password-sensitive.
- Rejection paths all behave: wrong password, flipped body bit, corrupt checksum, error flag, bad length, truncated input.
- Goldens verified identical across `-O0/-O1/-O2/-O3/-Os/-Ofast` on clang, which is the local check against the `/Og` class of bug that `sha.cpp` warns about.

**Still open:** no real `.sv` was involved, so this is *self-consistent*, not *verified against the retail game*. Two ways to close it, either sufficient:

- `BUTCHER_SAVE=/path/to/single_0.sv make check` — the Phase 2 suite reads `hero` straight out of the archive and asserts re-encode byte-identity. Easiest; no external tools.
- `BUTCHER_HERO_BLOB=./hero make check` — same assertion in the Phase 1 suite, against a 1288-byte blob extracted with `smpq -x single_0.sv hero`. Useful if the MPQ reader itself is suspect.

**Run one before trusting Phase 3 to write a save you care about.**

**Phase 2 — MPQ read. — DONE, pending real-save verification.**

`savefile.{h,cpp}` + the CLI; 42 checks in `tests/mpq_read.cpp`, all passing. `butcher dump <save.sv> [-o hero.bin]` decodes a save, prints the character, and optionally writes the raw 1266-byte `PkPlayerStruct`.

- Header validated against all eleven fields `ParseMPQHeader` checks, then **refused** on mismatch with a specific message. Saves are opened `"rb"` and never for writing; the suite asserts byte-for-byte file immutability after every one of the 11 rejection cases.
- Hash lookup mirrors `mpqapi_get_hash_index` exactly (probe from `Hash(name,0) & 0x7FF`, stop on `block == -1`, skip `-2`). Case-insensitivity falls out of `Hash`'s `toupper`, and is tested.
- Sector reading handles both storage forms. `PkwareCompress` (`Source/encrypt.cpp:103`) keeps the imploded output only when it is smaller, so a sector stored at full size is literal — the standard MPQ IMPLODE rule. Both paths are covered by feeding the reader compressible (`zeros`) and incompressible (`noise`) 3-sector files, plus 7-byte and exactly-4096-byte edge cases.
- `explode()` is driven through a local wrapper rather than `PkwareDecompress`, because that function discards the output length and its write callback is unbounded. The wrapper bounds-checks every write and returns the length, so a short, long, or overflowing sector is rejected instead of smashing the heap.
- 32 single-byte payload corruptions were probed; every one is caught by the decompressor, the length check, or the codec checksum. None silently yields a wrong character.

Deliberately unsupported, each with its own error message: per-file encryption, single-unit files, non-imploded storage, and any layout that is not the fixed Diablo one.

**Still open:** the suite builds its own archives. The builder is independent of the reader (it drives `Encrypt()` and `PkwareCompress()` directly and shares no logic with it), but a synthetic archive is not a retail one. `BUTCHER_SAVE=/path/to/single_0.sv make check` runs the real-save path: it reports the archive contents, decodes `hero`, and asserts that **re-encoding reproduces the archived bytes exactly** — which closes the Phase 1 real-save gate at the same time, with no external MPQ tool needed.

**Phase 3 — MPQ write. — DONE, pending real-save verification.**

`mpq_replace_file()` / `save_write_hero()` in `savefile.cpp`; 46 checks in `tests/mpq_write.cpp`, all passing. `butcher patch <save.sv> -i hero.bin [--no-backup]`.

The plan originally proposed rewriting in place when the new body fits the existing `sizealloc` and appending otherwise, mirroring `mpqapi_find_free_block`. **The implementation does something simpler and safer instead: rebuild-and-compact.** The whole archive is written afresh to a temp file, with every file *other* than the target copied as raw compressed bytes — never decompressed and recompressed. That removes the free-list allocator from the picture entirely, cannot lose a file whose sectors we fail to decode, and yields a deterministic, compact result. Growing or shrinking a file is then just a relayout, with no special case.

Properties the suite enforces, roughly in order of how badly a violation would hurt:

- **Failed writes change nothing.** Absent target, corrupt archive, missing file — each refuses, leaves the original byte-identical, and leaves no temp files. Writes go to `mkstemp` beside the target, are `fsync`ed, then **re-opened and read back and compared** before the `rename`. A file that would not read back is discarded rather than installed.
- **Untouched files survive byte-for-byte**, verified with `hero` deliberately placed mid-archive so neighbours shift on both sides if layout logic is wrong.
- **Tombstones survive.** A deleted hash entry (`block == -2`) keeps a probe chain intact; clearing one to `-1` would make an unrelated file silently unreachable. The suite plants a tombstone at `hero`'s home slot, moves `hero` one slot along, and checks it is still there and still reachable after a rewrite. The hash table is copied slot for slot; only block indices are remapped.
- **An unchanged rewrite reproduces the archive exactly** — byte-for-byte, and stable under repetition. `PkwareCompress` is the game's own routine and is deterministic, so a recompressed sector matches what the game would have written.
- **File permissions are preserved.** Found by running it: `mkstemp` creates at 0600 and `rename` carries that mode onto the target, silently stripping the save's permissions. Fixed with `fchmod` from the original's mode, and pinned by a test.
- **Multiplayer saves are refused rather than converted.** `save_read_hero` now reports which password worked; `patch` refuses if it was the multiplayer one, since `save_write_hero` only writes single-player encoding and re-keying would break the save where it came from.

`patch` writes `<save.sv>.bak` by default and **refuses if that file already exists** — so a second patch cannot destroy the pristine original.

**Still open:** same gap as before. `BUTCHER_SAVE=/path/to/single_0.sv make check` now also exercises the writer, on a *copy* — it rewrites the hero unchanged and asserts the result is byte-for-byte identical to the real archive. That is the strongest available evidence that the writer agrees with whatever wrote the save. The real save itself is never opened for writing.

**Phase 4 — The actual editor. — DONE, pending real-save verification.**

`hero.{h,cpp}` (limits, validation, gold) and `format.{h,cpp}` (rendering); 139 checks in `tests/editor.cpp`, all passing. Commands: `list`, `show`, `set`, plus the `dump`/`patch` escape hatch.

- **Limits come from the game.** `spelldat.cpp` and `itemdat.cpp` turn out to compile against the shim, so spell and base-item names are the real tables rather than a copy that would rot. `MaxStats` and `ExpLvlsTbl` could *not* be linked — `player.cpp` is one translation unit containing the whole player implementation — so they are copied into `hero.cpp` with provenance comments, and `NUM_CLASSES`, `MAXCHARLEVEL`, `ATTRIB_*` order and `IDI_GOLD == 0` are asserted against the real headers so a change breaks the build.
- **Gold is rewritten as inventory, not as a number.** `hero_set_gold` removes existing stacks, compacts `InvList`, remaps `InvGrid` (positive = origin, negative = continuation, zero = free), then lays down `ceil(total/5000)` stacks scanning cells 39→0 the way `AutoPlaceGold` does, and finally sets the cached `pGold` to match. Capacity is `free_cells * 5000`; over that is refused with the numbers shown. Every case is checked for inventory consistency — no dangling grid references, no live slot missing an origin cell, nothing non-empty above `_pNumInv`. Output is deterministic.
- **Experience and level interact.** `AddPlrExperience` (`Source/player.cpp:1074`) recomputes the level from experience starting at zero. It never lowers a level, so `--level 50` alone sticks — but the character can never level again until experience catches up. `set` warns with the exact `--exp` value that would make the level consistent. The reverse (high exp, low level) warns that the game will level the character up on its next kill.
- **Name collisions are checked.** `pfile_get_save_num_from_name` resolves a name to the first matching slot, so two saves sharing a name make one unreachable. `--name` scans sibling `.sv` files and refuses; `list` warns when it sees duplicates.
- Validation covers per-class stat caps, level 1–50, class range, negative/over-cap experience, life above maximum, life below 1 (the game rewrites it on load), dungeon level, `_pNumInv`, and names (empty, over-length, path separators, control characters). Out-of-range values are refused unless `--force`.

**Phase 5 — Polish. — Absorbed into phase 4.** Backups (refusing to overwrite an existing `.bak`), `--dry-run` diff output, and errors that distinguish "not an MPQ" / "no hero file" / "wrong password" / "unexpected size" were all built as they became relevant. One item was found only by running the tool: stdout is block-buffered when piped, so stderr warnings surfaced *before* the stdout diff they referred to; fixed with `setvbuf`.

**Phase 5 — Polish.** Backups, `--dry-run` diff output, clear errors distinguishing "not an MPQ", "no hero file", "wrong password / corrupt", "unexpected struct size".

## Test data

The repo ships no game assets and must keep it that way, so a real `.sv` cannot be committed. Options, in order of preference:

1. Generate a fixture at test time: if the tool can `patch`, it can also synthesize a minimal valid archive from a checked-in 1266-byte plaintext `PkPlayerStruct` (which contains no Blizzard data — it's the project's own struct layout). Round-trip against that.
2. Keep a checked-in *decoded* hero blob plus its expected 1288-byte ciphertext, so the crypto is tested without any MPQ.
3. Point tests at a local save via an env var, skipped when unset, for manual verification against the real game.

Use (1) and (2) for CI; use (3) plus an actual "load the edited character in Diablo and confirm it looks right" pass before calling any phase done. The end-to-end check that matters is: edit a character, launch the game, see the change, save, and confirm the game's own writer produces a file the editor still reads.

## Hellfire

Added after the fact, once a real Hellfire save turned up. The guess in the original plan was right: **the packed format is identical**, so this was purely an interpretation layer.

Measured, not assumed: `PkPlayerStruct` is 1266 bytes under both `-DHELLFIRE` and without, with `pName`, `pGold`, `pSplLvl`, `pSplLvl2` and `InvList` at the same offsets, and `SpellData`/`ItemDataStruct` the same size with no conditional members. `MAXCHARLEVEL` and `ExpLvlsTbl` are shared. Single-player saves use the same `"xrgyrkj1"` password. So `savefile.cpp` needed **no changes at all** — not one line in the MPQ, sector, or codec paths.

What actually differs, and how each is handled:

| Difference | Diablo | Hellfire | Approach |
| --- | --- | --- | --- |
| Classes | 3 | 6 (+Monk, Bard, Barbarian) | extra `MaxStats` rows, flavor-indexed |
| Spells defined | 37 | 52 | second `spelldata` table |
| Spells *persisted* | 0–36 | 0–46 | ids 37–46 route to `pSplLvl2`; 47+ refused |
| Dungeon levels | 17 | 25 | flavor-indexed `NUMLEVELS` |
| Item table | Diablo's | larger | second `AllItemsList` |

**Two data tables, twice, without editing them.** `Source/spelldat.cpp` and `Source/itemdat.cpp` are each compiled a second time with `-DHELLFIRE` *and the array name redefined on the command line* (`-Dspelldata=spelldata_hf`, `-DAllItemsList=AllItemsList_hf`). Both flavors' tables then coexist in one binary with no edit to either file — the same trick in spirit as the `__ALL_H__` guard. Hellfire's integer constants come from `compat/hellfire.cpp`, the one TU compiled with `-DHELLFIRE`, so `NUM_CLASSES`/`MAX_SPELLS`/`NUMLEVELS` are read from the real headers rather than typed in.

**Spells 47–51 are a genuine trap.** Hellfire's `MAX_SPELLS` is 52, but `PackPlayer` writes ids 0–36 to `pSplLvl[37]` and 37–46 to `pSplLvl2[10]` — so five spells exist in the game with nowhere to live in a save. `hero_set_spell_level` refuses them explicitly rather than writing past the array.

Flavor is inferred from the extension (`.hsv` → Hellfire) and overridable with `--hellfire`/`--diablo`. A Hellfire-only class found in a `.sv` produces a specific error rather than a wrong stat cap.

## JSON interchange

Added after the CLI was working, because `dump`/`patch` on a raw 1266-byte
struct is a poor interchange format — you cannot diff it, script it, or edit it
without a hex editor.

**The contract is losslessness**, and it is not optional: someone who exports a
character, changes one number, and imports has implicitly trusted the format
with the other 1265 bytes. Measuring the real saves first settled two things a
prettier schema would have got wrong:

- **Fractional life and mana are common.** A Monk at 54 life is stored as
  3458 = 54 + 2/64. Exporting whole points alone would have quietly altered
  every character that had regenerated since its last round number. The whole
  number is the editable field; the remainder sits beside it and is omitted
  when zero.
- **"Reserved" fields are not empty.** `bReserved[1] == 1` in both Hellfire
  Hellfire save examined. So `advanced` exists, holding the reserved and
  transient fields verbatim.

The load-bearing test fills the struct with **random bytes** and round-trips it,
400 times per run. A hand-written fixture only proves the fields the author
remembered; noise proves every field is represented. That test immediately
found a real defect: `read_class` used `-1` as its "unknown class" sentinel, but
`pClass` is a signed char, so a legitimate value of -111 was rejected — and on a
path that left the error message empty. Both fixed; success is now reported
separately from the value.

Verified against real saves of both games: they round-trip byte-for-byte.

**Format choice.** JSON, because the subproject has no external dependencies and
it is the only one of JSON/YAML/TOML that can be both written *and* parsed
correctly in a few hundred lines. The reader additionally accepts `//` and
`/* */` comments and trailing commas, since these files get hand-edited; the
writer never emits either, so its output stays strict JSON. TOML remains a
reasonable second format if hand-editing ergonomics matter more than tooling.

## Validation

`import` always ran the game-limit checks, but two whole classes of failure were
invisible, and both were measured on a real exported document before any code
was written:

- **A misspelled field imported as zero.** `"vitallity": 25` was silently
  ignored and `vitality` became 0. Indistinguishable from success.
- **An over-large value truncated.** `"strength": 300` imported as 44, because
  the field is one byte. No complaint.

Both are now errors. The mechanism is one table per object shape in
`charjson.cpp` that serves double duty: it declares which keys exist, so
anything else is a typo, and the range each value must fit, so a value that
would be truncated on the way into the struct is caught instead. Unknown keys
get a Levenshtein-based "did you mean", suppressed when no candidate is close
enough that a guess would help.

`hero_validate` used to stop at the first problem, which is the wrong shape for
a standalone checker — fixing one error to discover the next is a poor way to
repair a file. It is now a thin wrapper over `hero_check`, which appends every
finding to a `DiagList` and additionally verifies **inventory coherence**: grid
cells pointing at live items, live items reachable from the grid. `set` and
`import` report the full list too.

Two decisions worth recording:

- **Checking is staged like a compiler.** The document must be readable before
  the character it describes can be examined, and when the first stage stops
  things, validate says so explicitly. Silence about the second stage would
  otherwise read as approval.
- **One warning was removed for crying wolf.** Items sitting above
  `inventory.count` looked like a coherence problem, and the first run put eight
  warnings on a perfectly ordinary character. Real saves routinely carry residue
  there: `RemoveInvItem` decrements the count and shifts the list without
  clearing the vacated tail, and `PackPlayer` writes all 40 slots regardless.
  The game ignores it. A checker that flags normal data is how warnings get
  ignored, so it is gone; a grid cell pointing *past* the count is a different
  matter and remains an error.

The command is `validate` because that is the established term for checking a
document without applying it (`terraform validate`, `composer validate`);
`lint` would imply style advice.

**It takes any of the three formats, which was a correction.** `validate` shipped
accepting JSON only, and `validate single_0.sv` answered "unexpected character
'M'" -- true, since MPQ archives begin with M, and useless. Every other command
takes a save path, so that is the natural thing to type, and the question behind
it ("is this character valid") has an answer regardless of the container. Input
is now sniffed: the MPQ signature means a save, a length of exactly 1266 means a
raw struct, an early NUL means something unrecognized, and anything else is
parsed as JSON. Exit 2 is reserved for input that could not be read, so scripts
can tell a missing file from an invalid one.

## Open questions

- ~~**Hellfire.**~~ Done. See below.
- **Spawn (shareware) saves** use different passwords and filenames. Cheap to add (a `--spawn` flag selecting the password); worth doing only if someone wants it.
- **Multiplayer `.drv` files** may be keyed on `GetComputerName`, so a save is only readable on the machine that wrote it unless the user supplies the name. Support would mean a `--password` escape hatch.
