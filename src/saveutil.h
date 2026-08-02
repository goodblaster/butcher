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

#include "hero.h"

#define SAVE_PATH_MAX 1024
#define SAVE_MAX_SLOTS 20 /* ten Diablo plus ten Hellfire */

typedef struct SaveEntry {
	char path[SAVE_PATH_MAX];
	char name[PLR_NAME_LEN + 1];
	int slot;
	HeroFlavor flavor;
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

/**
 * Back up (optionally), write, then read back and compare.
 *
 * The read-back is the point: a save that does not survive a round trip is
 * never left in place. On any failure the original is untouched.
 */
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
