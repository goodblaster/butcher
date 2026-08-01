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
	char bak[SAVE_PATH_MAX];
	snprintf(bak, sizeof(bak), "%s.bak", path);

	FILE *in = fopen(path, "rb");
	if (in == NULL) {
		seterr(err, "cannot read %s", path);
		return 0;
	}
	/* "x" fails when the file exists, so an existing backup is never lost. */
	FILE *out = fopen(bak, "wbx");
	if (out == NULL) {
		fclose(in);
		seterr(err, "cannot create %s (it may already exist). Move it aside, "
		            "or save without a backup.",
		    bak);
		return 0;
	}

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
	if (backup && !save_backup(path, err))
		return 0;

	if (!save_write_hero(path, h, err))
		return 0;

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
