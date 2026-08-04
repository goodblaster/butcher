/**
 * @file saveutil.cpp
 *
 * Save discovery and safe writing. See saveutil.h.
 */
#include "saveutil.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <strings.h>
#include <sys/stat.h>

static void seterr(char *err, const char *fmt, ...)
{
	if (err == NULL)
		return;
	va_list va;
	va_start(va, fmt);
	vsnprintf(err, MPQ_ERR_LEN, fmt, va);
	va_end(va);
}

HeroFlavor save_flavor_of(const char *path)
{
	return hero_flavor_for_path(path);
}

/* ------------------------------------------------------------------ */
/* Discovery                                                           */
/* ------------------------------------------------------------------ */

int save_default_dirs(char out[][SAVE_PATH_MAX], int max)
{
	const char *home = getenv("HOME");
	int n = 0;

	if (home == NULL)
		return 0;

	/*
	 * DevilutionX keeps saves under the platform's application-data
	 * directory. The retail game keeps them beside Diablo.exe, which we
	 * cannot guess, so a directory argument stays supported everywhere.
	 */
	static const char *relative[] = {
		"/Library/Application Support/diasurgical/devilution", /* macOS */
		"/.local/share/diasurgical/devilution",                /* Linux */
		"/AppData/Roaming/diasurgical/devilution",             /* Windows-ish */
		"/.wine/drive_c/Program Files/Diablo",
	};

	for (size_t i = 0; i < sizeof(relative) / sizeof(relative[0]) && n < max; i++) {
		struct stat st;
		char path[SAVE_PATH_MAX];
		snprintf(path, sizeof(path), "%s%s", home, relative[i]);
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
			snprintf(out[n++], SAVE_PATH_MAX, "%s", path);
	}
	return n;
}

int save_scan_dir(const char *dir, SaveEntry *out, int max)
{
	static const char *exts[2] = { ".sv", ".hsv" };
	int n = 0;

	for (int e = 0; e < 2; e++) {
		for (int i = 0; i < MAX_CHARACTERS && n < max; i++) {
			char path[SAVE_PATH_MAX];
			snprintf(path, sizeof(path), "%s/single_%d%s", dir, i, exts[e]);

			PkPlayerStruct h;
			char err[MPQ_ERR_LEN];
			if (!save_read_hero(path, &h, NULL, err))
				continue;

			SaveEntry *s = &out[n++];
			snprintf(s->path, sizeof(s->path), "%s", path);
			memcpy(s->name, h.pName, PLR_NAME_LEN);
			s->name[PLR_NAME_LEN] = '\0';
			s->slot = i;
			s->flavor = hero_flavor_for_path(path);
			s->in_progress = save_has_game(path);
			s->hero = h;
		}
	}
	return n;
}

int save_scan_default(SaveEntry *out, int max, char *used_dir)
{
	char dirs[8][SAVE_PATH_MAX];
	int ndirs = save_default_dirs(dirs, 8);

	for (int i = 0; i < ndirs; i++) {
		int n = save_scan_dir(dirs[i], out, max);
		if (n > 0) {
			if (used_dir != NULL)
				snprintf(used_dir, SAVE_PATH_MAX, "%s", dirs[i]);
			return n;
		}
	}
	if (used_dir != NULL && ndirs > 0)
		snprintf(used_dir, SAVE_PATH_MAX, "%s", dirs[0]);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */

int save_backup(const char *path, char *err)
{
	return save_backup_to(path, NULL, 0, err);
}

int save_backup_to(const char *path, char *chosen, size_t chosen_len, char *err)
{
	char bak[SAVE_PATH_MAX];

	FILE *in = fopen(path, "rb");
	if (in == NULL) {
		seterr(err, "cannot read %s", path);
		return 0;
	}

	/*
	 * "x" fails when the file exists, so an existing backup is never
	 * overwritten -- but refusing outright meant losing the edit instead. Fall
	 * back to .bak.1, .bak.2, ... so the older copy stays put and the save
	 * still proceeds.
	 */
	FILE *out = NULL;
	for (int i = 0; i <= SAVE_BACKUP_TRIES; i++) {
		if (i == 0)
			snprintf(bak, sizeof(bak), "%s.bak", path);
		else
			snprintf(bak, sizeof(bak), "%s.bak.%d", path, i);
		out = fopen(bak, "wbx");
		if (out != NULL)
			break;
		if (errno != EEXIST) {
			fclose(in);
			seterr(err, "cannot create %s: %s", bak, strerror(errno));
			return 0;
		}
	}
	if (out == NULL) {
		fclose(in);
		seterr(err, "%s.bak and %s.bak.1 through .%d all exist; remove some of "
		            "them, or save without a backup",
		    path, path, SAVE_BACKUP_TRIES);
		return 0;
	}
	if (chosen != NULL)
		snprintf(chosen, chosen_len, "%s", bak);

	char buf[8192];
	size_t n;
	int ok = 1;
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) {
			ok = 0;
			break;
		}
	}
	if (ferror(in))
		ok = 0;
	if (fclose(out) != 0)
		ok = 0;
	fclose(in);

	if (!ok) {
		remove(bak);
		seterr(err, "failed while writing %s", bak);
		return 0;
	}
	return 1;
}

int save_commit(const char *path, const PkPlayerStruct *h, int backup, char *err)
{
	return save_commit_ex(path, h, backup, NULL, err);
}

int save_commit_ex(const char *path, const PkPlayerStruct *h, int backup,
    SaveGameSync *sync, char *err)
{
	if (sync != NULL)
		*sync = SAVE_GAME_ABSENT;

	/*
	 * Do all the work that can fail before touching the file. Locating the
	 * character inside the saved game is the part most likely to refuse, and
	 * failing after "hero" was already rewritten would leave exactly the
	 * half-applied edit this exists to prevent.
	 */
	PkPlayerStruct old;
	if (!save_read_hero(path, &old, NULL, err))
		return 0;

	DWORD glen = 0;
	int present = 0;
	char gerr[MPQ_ERR_LEN] = { 0 };
	BYTE *game = game_read(path, &glen, &present, gerr);
	if (present && game == NULL) {
		seterr(err, "%s", gerr);
		return 0;
	}

	GameLoc loc;
	int items_moved = 0;
	int items_synced = 0;
	if (game != NULL) {
		/* Verified against the character as it is on disk, not as it is being
		 * changed to -- the two copies agree only before the edit. */
		if (!game_locate(game, glen, &old, &loc, gerr)) {
			free(game);
			seterr(err, "%s", gerr);
			return 0;
		}
		game_apply(game, glen, &loc, h);

		/*
		 * The inventory only when it actually moved. Gold is the reason this
		 * matters: ValidatePlayer recomputes the total from the stacks every
		 * tick, so a gold change that reaches only the packed copy is undone
		 * on the first frame of play.
		 */
		items_moved = memcmp(old.InvBody, h->InvBody, sizeof(old.InvBody)) != 0
		    || memcmp(old.InvList, h->InvList, sizeof(old.InvList)) != 0
		    || memcmp(old.InvGrid, h->InvGrid, sizeof(old.InvGrid)) != 0
		    || memcmp(old.SpdList, h->SpdList, sizeof(old.SpdList)) != 0
		    || old._pNumInv != h->_pNumInv;

		if (items_moved) {
			if (game_apply_inventory(game, glen, &loc, &old, h, gerr) < 0) {
				free(game);
				seterr(err, "%s", gerr);
				return 0;
			}
			items_synced = 1;
		}
	}

	if (backup && !save_backup(path, err)) {
		free(game);
		return 0;
	}

	if (!save_write_hero(path, h, err)) {
		free(game);
		return 0;
	}

	if (game != NULL) {
		int ok = game_write(path, game, glen, gerr);
		free(game);
		if (!ok) {
			seterr(err, "the character was written but the saved game was not: "
			            "%s. The save now disagrees with itself -- restore the "
			            "backup.",
			    gerr);
			return 0;
		}
		if (sync != NULL)
			*sync = items_synced ? SAVE_GAME_SYNCED_ITEMS : SAVE_GAME_SYNCED;
	}

	/* Never leave a save in place that does not read back as written. */
	PkPlayerStruct after;
	if (!save_read_hero(path, &after, NULL, err))
		return 0;
	if (memcmp(&after, h, sizeof(after)) != 0) {
		seterr(err, "the save read back differently from what was written; "
		            "do not trust this file");
		return 0;
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

static void dirname_of(const char *path, char *out, size_t n)
{
	const char *slash = strrchr(path, '/');
	if (slash == NULL) {
		snprintf(out, n, ".");
		return;
	}
	size_t len = (size_t)(slash - path);
	if (len == 0)
		len = 1;
	if (len >= n)
		len = n - 1;
	memcpy(out, path, len);
	out[len] = '\0';
}

static int same_leaf(const char *a, const char *b)
{
	const char *ab = strrchr(a, '/');
	const char *bb = strrchr(b, '/');
	return strcmp(ab ? ab + 1 : a, bb ? bb + 1 : b) == 0;
}

int save_name_collides(const char *save_path, const char *name, char *other, size_t n)
{
	char dir[SAVE_PATH_MAX];
	dirname_of(save_path, dir, sizeof(dir));

	DIR *d = opendir(dir);
	if (d == NULL)
		return 0;

	int hit = 0;
	struct dirent *e;
	while (!hit && (e = readdir(d)) != NULL) {
		size_t l = strlen(e->d_name);
		int is_sv = (l >= 4 && strcasecmp(e->d_name + l - 3, ".sv") == 0);
		int is_hsv = (l >= 5 && strcasecmp(e->d_name + l - 4, ".hsv") == 0);
		if (!is_sv && !is_hsv)
			continue;

		char full[SAVE_PATH_MAX * 2];
		snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
		if (same_leaf(full, save_path))
			continue;

		PkPlayerStruct oh;
		char err[MPQ_ERR_LEN];
		if (!save_read_hero(full, &oh, NULL, err))
			continue;

		char on[PLR_NAME_LEN + 1];
		memcpy(on, oh.pName, PLR_NAME_LEN);
		on[PLR_NAME_LEN] = '\0';
		if (strcasecmp(on, name) == 0) {
			snprintf(other, n, "%s", e->d_name);
			hit = 1;
		}
	}
	closedir(d);
	return hit;
}
