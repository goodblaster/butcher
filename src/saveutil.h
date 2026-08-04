/**
 * @file saveutil.h
 *
 * Things both front ends need: finding saves, backing one up, committing a
 * change, and checking a rename against its siblings.
 *
 * This exists so the TUI does not shell out to the CLI. Both link the same
 * library; neither drives the other.
 */
#ifndef BUTCHER_SAVEUTIL_H
#define BUTCHER_SAVEUTIL_H

#include "gamefile.h"
#include "hero.h"

#define SAVE_PATH_MAX 1024
#define SAVE_MAX_SLOTS 20 /* ten Diablo plus ten Hellfire */

typedef struct SaveEntry {
	char path[SAVE_PATH_MAX];
	char name[PLR_NAME_LEN + 1];
	int slot;
	HeroFlavor flavor;
	/**
	 * The archive holds a game in progress, so the character it loads comes
	 * from "game" rather than from the "hero" this edits. See save_has_game.
	 */
	int in_progress;
	PkPlayerStruct hero;
} SaveEntry;

/**
 * Where the games keep saves on this platform, most likely first.
 * @return how many were written to @p out.
 */
int save_default_dirs(char out[][SAVE_PATH_MAX], int max);

/**
 * Read every `single_N.sv` and `single_N.hsv` in @p dir that decodes.
 * @return how many were found.
 */
int save_scan_dir(const char *dir, SaveEntry *out, int max);

/** Scan the platform defaults, in order, until one yields saves. */
int save_scan_default(SaveEntry *out, int max, char *used_dir);

/** Infer Diablo or Hellfire from a path's extension. */
HeroFlavor save_flavor_of(const char *path);

/** How many numbered fallbacks save_backup will try after `<path>.bak`. */
#define SAVE_BACKUP_TRIES 99

/**
 * Copy @p path to `<path>.bak`, or to `<path>.bak.1`, `.bak.2`, ... when
 * earlier names are taken.
 *
 * An existing backup is never overwritten -- the first one is the most
 * pristine copy and the one worth keeping. Earlier versions refused to write
 * at all once `.bak` existed, which cost the edit rather than the backup.
 */
int save_backup(const char *path, char *err);

/**
 * As save_backup, but reports the name it settled on in @p chosen so the
 * caller can tell the user where the copy went.
 */
int save_backup_to(const char *path, char *chosen, size_t chosen_len, char *err);

/** What happened to the "game" copy of the character during a commit. */
typedef enum SaveGameSync {
	SAVE_GAME_ABSENT = 0, /**< no game in progress; nothing to do */
	SAVE_GAME_SYNCED,     /**< the saved game was updated to match */
	/** Updated, including the inventory -- so gold changes hold. */
	SAVE_GAME_SYNCED_ITEMS,
} SaveGameSync;

/**
 * Back up (optionally), write, then read back and compare.
 *
 * The read-back is the point: a save that does not survive a round trip is
 * never left in place. On any failure the original is untouched.
 *
 * When the archive holds a game in progress, the character inside it is
 * updated too -- editing only "hero" would show on the character-selection
 * screen and then be discarded on load. If that copy cannot be located and
 * verified, the whole commit fails and nothing is written: a partial edit here
 * is worse than none, because it looks like it worked.
 *
 * @param sync optional; reports which of those two happened.
 */
int save_commit_ex(const char *path, const PkPlayerStruct *h, int backup,
    SaveGameSync *sync, char *err);

/** save_commit_ex without the report. */
int save_commit(const char *path, const PkPlayerStruct *h, int backup, char *err);

/**
 * The game resolves a character name to the first matching save slot, so two
 * characters sharing a name makes one unreachable.
 *
 * @param other receives the conflicting file's name.
 * @return nonzero if some other save beside @p save_path already uses @p name.
 */
int save_name_collides(const char *save_path, const char *name, char *other, size_t n);

#endif /* BUTCHER_SAVEUTIL_H */
