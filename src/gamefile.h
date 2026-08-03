/**
 * @file gamefile.h
 *
 * Editing the character inside a saved game.
 *
 * A save carries the character twice. "hero" is the packed PkPlayerStruct that
 * savefile.h deals with, and it is what the character-selection screen shows.
 * "game" is present whenever a game is in progress and holds a full
 * PlayerStruct; selecting the character runs LoadGame, which calls LoadPlayer
 * and overwrites the player from it. Editing only "hero" therefore shows on the
 * selection screen and is discarded the moment play starts.
 *
 * The player record inside "game" is the raw 32-bit PlayerStruct as it sat in
 * memory: the original does `memcpy(&plr[i], tbuff, ...)` (devilution
 * Source/loadsave.cpp), and DevilutionX's field-by-field reader with its
 * Skip(4) calls for pointers and alignment is a description of that same
 * layout. Retail and DevilutionX agree, so there is one format here, not two.
 *
 * Nothing changes size, so edits are byte patches and the rest of the file --
 * monsters, objects, dungeon, items on the ground -- never has to be parsed.
 *
 * On not trusting offsets
 * -----------------------
 * Every offset derived by reading the game's source turned out wrong: the
 * prefix by 12 bytes (a DevilutionX revision three months from the build being
 * targeted), the name by 16 (PLR_NAME_LEN is 32, not 16), and the gap between
 * the spell array and _pMemSpells by 5. Patching 200 KB of game state at a
 * wrong offset is unrecoverable.
 *
 * So no offset here is trusted. The record is found by matching the character
 * name, and then every field is compared against "hero" before a single byte is
 * written. The game writes both members together, so on a healthy save they
 * agree -- which makes that comparison a free validity proof, per file and per
 * game version. A layout that has shifted fails the check instead of corrupting
 * the save.
 */
#ifndef BUTCHER_GAMEFILE_H
#define BUTCHER_GAMEFILE_H

#include "savefile.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Where the player record sits, once found and verified. */
typedef struct GameLoc {
	/** Offset of _pName within the decoded blob. Every field is relative. */
	long name;
	/** Dungeon levels this format carries: 17 for Diablo, 25 for Hellfire. */
	int levels;
	int hellfire;
	/**
	 * Fields that agreed with "hero" and were not zero on both sides.
	 *
	 * Zero-on-both proves nothing -- a wrong offset landing on padding would
	 * also read zero -- so a location is only accepted with several of these.
	 */
	int discriminating;
} GameLoc;

/** How many non-trivial field agreements a location must show to be trusted. */
#define GAME_MIN_DISCRIMINATING 4

/**
 * Read and decode the "game" member.
 *
 * @return the decoded bytes, or NULL when the save holds no game in progress
 *         (which is not an error -- @p err is set only on a real failure).
 *         Caller frees.
 */
BYTE *game_read(const char *path, DWORD *out_len, int *present, char *err);

/**
 * Find the player record and verify it against @p hero.
 *
 * Fails rather than guesses: refuses when the name matches nowhere, when it
 * matches in more than one place that also passes verification, when any field
 * disagrees, or when too few fields are discriminating.
 *
 * @return nonzero on success.
 */
int game_locate(const BYTE *buf, DWORD len, const PkPlayerStruct *hero,
    GameLoc *loc, char *err);

/**
 * Write @p hero's values into the located record.
 * @return how many fields actually changed.
 */
int game_apply(BYTE *buf, DWORD len, const GameLoc *loc, const PkPlayerStruct *hero);

/** Encode and replace the "game" member of the archive. */
int game_write(const char *path, const BYTE *buf, DWORD len, char *err);

#ifdef __cplusplus
}
#endif

#endif /* BUTCHER_GAMEFILE_H */
