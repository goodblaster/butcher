# butcher

A character editor for **Diablo** and **Hellfire** save files, as a command-line
tool and a terminal UI.

Reads `single_N.sv` and `single_N.hsv`, shows you what is in them, edits them,
and round-trips a character through JSON so you can change it in a text editor
or a script. No GUI, no game assets, no Wine.

```
$ butcher list
slot  name                 game      class     level   gold
   2  Aidan                Diablo    Warrior   lvl 12       2450 gold
   0  Kaelan               Hellfire  Monk      lvl 24      18300 gold
   1  Moreina              Hellfire  Rogue     lvl 8          860 gold
```

Named after the boss who greets you on dungeon level 2 and, traditionally,
ends the run right there.

> butcher could not exist without
> [**devilution**](https://github.com/diasurgical/devilution), the
> reverse-engineered reconstruction of Diablo's original 1996 source code. The
> save format is documented nowhere; it was recovered by reverse engineering,
> and butcher compiles seven of those files **unmodified** rather than
> reimplementing them — the encryption, the compression, and the structure
> definitions. Everything above that line, from the archive handling to the two
> interfaces, is butcher's own. See [Credits](#credits).

## Build

```bash
git clone --recurse-submodules git@github.com:goodblaster/butcher.git
cd butcher
make check
```

Already cloned without submodules? `git submodule update --init`.

To put it on your `PATH`:

```bash
sudo make install              # /usr/local/bin/butcher
make install PREFIX=~/.local   # or somewhere you own, no sudo
make uninstall
```

Needs a C++17 compiler and nothing else — no external libraries, no CMake, no
package manager. `make check` builds the tool and runs 559 tests.

This produces one binary, `build/butcher`, carrying two interfaces.

**Naming a subcommand selects the command line; anything else opens the
terminal UI.**

```bash
butcher                      # browse your saves
butcher single_0.hsv         # open one character
butcher show single_0.hsv    # command line
```

Both front ends link `src/` directly. **Neither invokes the other** — there is
no subprocess, no serialization boundary, and nothing to keep in sync. The
dispatch in `src/butcher.cpp` only chooses which one runs.

If you ever have a save named after a subcommand, `--tui` forces the interface.

## Verify it against one of your saves first

The tests pass against archives the tool builds itself. Before you let it edit
a character you care about, point it at a real one:

```bash
BUTCHER_SAVE=/path/to/single_0.sv make check
```

This decodes the save, asserts that **re-encoding reproduces the archived bytes
exactly**, round-trips the character through JSON byte-for-byte, and exercises
the writer on a *copy*. It takes seconds and it is the only check that proves
this build agrees with the game rather than merely with itself. Your save is
never opened for writing.

## The terminal UI

```bash
butcher                      # saves in the current directory
butcher --saves              # the game's own save folder
butcher <save>               # open one directly
butcher <dir>                # browse a particular directory
```

**Naming a save file opens it; anything that scans a directory shows the
picker** — even when the directory holds exactly one character. Landing
straight in a character you did not choose is the tool picking for you, and
with one save in the folder you would not notice it had.

A bare `butcher` looks **only in the current directory**. If there is nothing
there it tells you where your game keeps its saves and how to open them, but it
will not reach into that folder uninvited — your working directory is the only
context it assumes.

It opens on a picker, or straight into a character sheet with three panes —
**Attributes**, **Spells**, **Inventory**.

```
╭────────────────────────────────────────────────────────────────────────────╮
│ Kaelan                                                       Hellfire Monk │
├──────────┬──────┬──────────────────────────────────────────────────────────┤
│Attributes│Spells│Inventory                                                 │
├──────────┴──────┴──────────────────────────────────────────────────────────┤
│  Name       Kaelan                                                         │
│  Class      ○ Warrior                                                      │
│             ○ Rogue                                                        │
│             ○ Sorcerer                                                     │
│             ◉ Monk                                                         │
│             ○ Bard                                                         │
│             ○ Barbarian                                                    │
├────────────────────────────────────────────────────────────────────────────┤
│  Strength   90   ██████████████████████████████▌                      0-150│
│▸ Magic      55   ███████████████████████████████████▋                  0-80│
│  Dexterity  110  █████████████████████████████████████▎               0-150│
│  Vitality   70   █████████████████████████████████████████████▍        0-80│
│  Unspent    0                                                         0-255│
├────────────────────────────────────────────────────────────────────────────┤
│  Level      24   ████████████████████████▎                             0-50│
│  Experience 3,500,000                                                      │
├────────────────────────────────────────────────────────────────────────────┤
│  Life       148  ███████████████████████████████████████████████████  0-148│
│  Life max   148  ████████████████████████████████▏                    0-233│
│  Mana       96   ████████████████████████████████████████████████████  0-96│
│  Mana max   96   ██████████████████████▍                              0-218│
├────────────────────────────────────────────────────────────────────────────┤
│  Gold       18,300    ████▌                                       0-165,000│
│   └ 4 stacks of at most 5,000, 29 cells free                               │
│  Dungeon    12   ██████████████████████████                            0-24│
│                                                                            │
│                                                                            │
│                                                                            │
│                                                                            │
│                                                                            │
│                                                                            │
│                                                                            │
├────────────────────────────────────────────────────────────────────────────┤
│  ✓ valid                                                                   │
├────────────────────────────────────────────────────────────────────────────┤
│ unchanged  backup on                                                 0E 0W │
│ tab pane · ↑↓ field · ←→ adjust · ^S save · ^F fix · ^R revert · q quit    │
╰────────────────────────────────────────────────────────────────────────────╯
```

`tab` cycles panes, arrows move between fields and adjust them, `space` toggles
a spell in the book, `^S` saves behind a confirmation, `^F` repairs anything
invalid, `^R` reverts, `^B` toggles whether a backup is written, `^U` clears the
name field, `esc` returns to the picker, and `q` quits. The `▸` marks the field
you are on.

The sheet will not save a character that fails validation, so `^F` is the way
out of one: it applies the same repairs as `butcher fix --all`, shows what it
changed, and writes nothing — `^S` still saves and `^R` still throws it away.

**Every limit is the control, not a label.** The slider bounds are
`hero_max_stat()` — switch a character to Barbarian and the magic slider becomes
immovable, because a Barbarian's magic cap really is 0. The strip above the
status line is `hero_check()`, so it reports exactly what `butcher validate`
would.

`--render` draws one frame to stdout and exits — useful in a pipe or a bug
report, and how the frame above was produced. `--tab N` selects a pane and
`--focus N` places the cursor before rendering.

## Use

```bash
butcher list     [dir]                     # what is in each save slot
butcher show     <save>                    # full character sheet
butcher validate <file>                    # check anything, change nothing
butcher set      <save> [options]          # edit from the command line
butcher export   <save> [-o char.json]     # character out, as JSON
butcher import   <save> -i char.json       # character in, from JSON
butcher dump     <save> [-o hero.bin]      # raw 1266-byte struct out
butcher patch    <save> -i hero.bin        # raw struct in
```

### Editing

```bash
butcher set single_0.hsv --level 30 --gold 25000 --dry-run   # preview
butcher set single_0.hsv --level 30 --gold 25000
```

Options: `--name --class --level --exp --statpts --str --mag --dex --vit
--hp --maxhp --mana --maxmana --gold --dlvl`, plus `--spell NAME=LEVEL`
(repeatable; level 0 forgets it — `NAME` may also be a numeric id, which is
how you clear one the save should not be carrying). Modifiers: `--dry-run`,
`--force`, `--raw`, `--no-backup`.

### JSON

`export` writes the whole character; `import` reads it back. The round-trip is
**lossless** — all 1266 bytes — so you can change one number without disturbing
anything else.

```bash
butcher export single_0.hsv -o monk.json
$EDITOR monk.json
butcher validate monk.json
butcher import single_0.hsv -i monk.json
```

`-o -` writes to stdout, so `jq` works:

```bash
butcher export single_0.hsv -o - | jq -r '"\(.name): level \(.level)"'
butcher export single_0.hsv -o - | jq '.level = 30' | butcher validate -
```

On input the reader also accepts `//` and `/* */` comments and trailing commas,
since these files get hand-edited. Nothing it writes uses either.

### Validating

```bash
butcher validate single_0.hsv     # a save
butcher validate char.json        # a JSON document
butcher validate hero.bin         # a raw struct
```

Takes whichever form the character is in. Exit status is `0` clean, `1` if the
character or document has problems, `2` if the input could not be read at all —
so a missing file is distinguishable from an invalid one in a script.
`--strict` also fails on warnings; `--quiet` sets only the exit code.

Every finding is reported at once, with a location:

```
$ butcher validate broken.json
broken.json:attributes.magick: error: unknown field "magick" -- did you mean
    "magic"? As written it is ignored, and "magic" imports as zero
broken.json:attributes.strength: error: 300 is outside 0..255; the save stores
    this field in too few bits and the value would be truncated

2 errors, 0 warnings
```

Only JSON has a document to get wrong, so the key and range checks apply to it
alone; a save or a raw struct gets the character checks only.

### Repairing

```bash
butcher fix single_0.sv              # make it loadable
butcher fix single_0.sv --all        # and settle the warnings too
butcher fix single_0.sv --dry-run    # just say what it would do
```

Repairs what `validate` reports, but only where the right answer is not a
guess: values are clamped into the range the game accepts, and state the game
cannot represent is cleared. It never invents a character — a stat over its cap
comes down to the cap, not to something plausible, and items are never
synthesized. Every change is listed:

```
$ butcher fix single_0.sv
single_0.sv:spells: fixed: spell id 37 does not exist in Diablo; cleared
single_0.sv:attributes.strength: fixed: 200 was above the Sorcerer cap of 45, now 45

2 changes
backed up to single_0.sv.bak
saved.
```

By default it leaves warnings alone, since the game corrects those itself and
the character loads either way. `--all` settles them as well. If the result
would still not load, nothing is written and it says so.

One case is a guess and says so loudly: a class the game does not have has no
recoverable original, so it resets to Warrior and tells you to set it yourself.

## Things the game does that will surprise you

These are the reasons this tool exists rather than a hex editor.

**Gold is not a field.** The game recomputes it from the gold piles in your
inventory on nearly every inventory action, so writing the cached total lasts
until you pick up a coin. `--gold` redistributes actual stacks — 5000 per pile,
so 40 empty cells hold 200,000 — and refuses when there is not enough room.

**Level and experience interact, and experience is the real field.**
`AddPlrExperience` recomputes the level from experience and calls
`NextPlrLevel` once per level gained — which is where the rewards are: +5 stat
points, +128 max life raw (+64 for a Sorcerer), matching mana, and a full heal.
Writing the level field directly grants none of that *and* leaves the character
unable to level again. So the TUI's Level slider writes **experience** and lets
the game award the levels properly; the CLI warns and tells you the `--exp`
value that does the same.

**Names must be unique across saves.** The game resolves a name to the first
matching save slot, so two characters sharing one makes the other unreachable.
`--name` checks the siblings and refuses; `list` warns about duplicates.

**Life and mana are fixed point.** Six fractional bits — a Monk at 54 life is
stored as 3458, which is 54 + 2/64. The CLI takes whole points; the JSON keeps
the remainder beside it so a round-trip is exact.

**Items are seed-based and are not editable.** A packed item stores an RNG seed
and a create-info word; the game regenerates the item by replaying its own
generator. There is no "+3 fire damage" field to change. Items are shown and
round-tripped faithfully, never synthesized.

**Spell ids are not interchangeable between the two games.** Diablo's spell
table has 37 rows and Hellfire's 52, and the game indexes that table *directly*
from the character's spell fields while drawing the spell book. A Hellfire id
in a Diablo save is therefore not a spell the game ignores — it is a read past
the end of an array, and the character may simply stop appearing in the
character list. butcher refuses to write one, and `validate` reports it as an
error. Clearing one is always allowed, so a save that already has the problem
can be repaired: `butcher set <save> --spell 37=0`.

**Not every spell can be in the spell book.** `sBookLvl` is -1 for the class
skills — Repair, Disarm, Recharge, Search — and for staff-only spells like
Mana and the Jester. The game grants those through the class and masks its
spell-book field down to book spells every time it loads a character, so a bit
set for one of them disappears on the next load. butcher will give them a level
but keeps them out of the book.

## Safety

- Reads open the file read-only. The game truncates a save it does not
  recognize; this never does.
- Writes go to a temporary file which is re-opened, read back, and compared
  before it replaces the original. Anything that fails verification leaves the
  save untouched.
- `set`, `import` and `patch` write a backup first. The first one is
  `<save>.bak`; later edits go to `<save>.bak.1`, `.bak.2`, and so on. An
  existing backup is **never overwritten**, so `<save>.bak` always holds the
  oldest copy — the one furthest from whatever you just broke.
- File permissions are preserved.
- Multiplayer-keyed heroes are refused rather than silently re-encoded.

To restore: `cp single_0.sv.bak single_0.sv`. Every write names the backup it
made, so check that line if you want a more recent one.

## Diablo and Hellfire

Both, inferred from the extension — `.hsv` is Hellfire, `.sv` is Diablo, and
`--hellfire` / `--diablo` override it. The two share the packed save format
byte for byte, so only the interpretation differs: Hellfire adds Monk, Bard and
Barbarian with their own stat caps, ten more usable spells, and eight more
dungeon levels.

Not supported: multiplayer `.drv` files, shareware saves, and editing the level
data inside an archive.

## Layout

| Path | |
| --- | --- |
| `src/` | The shared library — format, validation, editing, discovery |
| `src/compat/` | Win32 types, msvcrt `rand`, Hellfire constants |
| `cli/` | The command-line front end |
| `tui/` | The terminal front end (FTXUI) |
| `src/butcher.cpp` | Chooses between them |
| `tests/` | Nine suites, 559 checks |
| `third_party/devilution` | Submodule; see below |
| `third_party/ftxui` | Submodule; the terminal UI library |
| `docs/DESIGN.md` | Why it is built the way it is |

## The devilution submodule

butcher compiles seven files from
[diasurgical/devilution](https://github.com/diasurgical/devilution)
**unmodified**: `sha.cpp`, `codec.cpp`, `encrypt.cpp`, `spelldat.cpp`,
`itemdat.cpp`, and PKWare's `explode.cpp`/`implode.cpp`, plus the root headers
that define the save format.

That is deliberate, and the reason there is a submodule rather than a vendored
copy. `sha.cpp` is *X-SHA-1* — a broken SHA-1 with three independent deviations
from the real thing, and a documented hazard where compiler optimization
changes the digest and produces saves the game rejects. The key derivation in
`codec.cpp` depends on the exact PRNG in Microsoft's C runtime. Neither can be
safely rewritten from a description; the only reason to trust them is that they
are the same code the game runs, checked byte-for-byte against a real save.

`src/compat/shim.h` makes them build off Windows without editing them: it
force-defines their include guard so `#include "all.h"` expands to nothing, and
supplies the handful of Win32 types and an msvcrt-compatible `rand`.

**Nothing under `third_party/` may be edited.** It is a byte-accuracy
reconstruction of the original game; a change there is a change to the meaning
of every save file.

The people who wrote it are named in [Credits](#credits).

## Credits

butcher exists because other people did the hard part first. The save format is
not documented anywhere; it was recovered by reverse engineering, and this tool
compiles that reconstruction rather than reimplementing it.

### [devilution](https://github.com/diasurgical/devilution)

The reconstruction of Diablo's original source code, by the
[**diasurgical**](https://github.com/diasurgical) organisation and
[65 contributors](https://github.com/diasurgical/devilution/graphs/contributors).

- [**GalaXyHaXz**](https://github.com/galaxyhaxz) — created devilution, reverse
  engineering Diablo from PSX debug symbols and a leftover debug build over four
  months in 2018. The project, and therefore this one, starts with him.
- [**Anders Jenbo** (AJenbo)](https://github.com/AJenbo) — the largest
  contributor to devilution by a wide margin, and the top author on *every
  single file* butcher compiles: `sha.cpp`, `codec.cpp`, `encrypt.cpp`,
  `spelldat.cpp`, `itemdat.cpp`, and the `structs.h`/`defs.h`/`enums.h` headers
  that define the save format.
- [**qndel**](https://github.com/qndel) — extensive work across the codebase,
  including `itemdat.cpp` and the enums butcher depends on.
- [**Robin Eklind** (mewmew)](https://github.com/mewmew) and the
  [**sanctuary**](https://github.com/sanctuary) project — documenting Diablo's
  engine and converting the PSX `.SYM` symbols into usable headers, which is
  how the structures butcher relies on came to be named at all.
- **Sergey Semushin**, **Dennis Duda**, **staphen**, **squidcc**, **pionere**
  and everyone else who touched the specific files here.

`PkPlayerStruct` — the 1266-byte structure this entire tool is organised
around — is their reconstruction. So are the X-SHA-1 implementation and the
save codec, neither of which could have been guessed from the outside.

### PKWare compression

[**Ladislav Zezula**](https://github.com/ladislav-zezula) — `explode.cpp` and
`implode.cpp` are his implementation of the PKWARE Data Compression Library
(2003), used to decompress and recompress the sectors inside a save archive. He
also documented Storm, the library the game uses for MPQ archives.

### DevilutionX

[**DevilutionX**](https://github.com/diasurgical/devilutionX) is the modern,
cross-platform port most people actually play, and where the saves butcher was
tested against came from. Same organisation, same lineage.

### And the obvious one

Blizzard North made the game in 1996. None of this exists without that.

## Licence

Not yet chosen. Note that devilution is under the **Sustainable Use License**,
which limits use to non-commercial or personal purposes and requires that
copyright notices be preserved — worth reading before publishing anything built
on it. PKWare's `explode.cpp`/`implode.cpp` are Ladislav Zezula's, under their
own notice. Please keep both intact.

## Status

Working and tested, not yet released. Everything above is implemented.

Ideas not done: TOML as a second text format, Hellfire `.hsv` multiplayer
saves, and a `--sync-exp` convenience for the level/experience trap.
