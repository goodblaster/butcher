/**
 * @file main.cpp
 *
 * Character editor CLI: list, show, set, dump, patch.
 */
#include "../src/charjson.h"
#include "../src/format.h"
#include "../src/saveutil.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <strings.h> /* strcasecmp */

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

static void warn_line(const char *msg)
{
	fprintf(stderr, "warning: %s\n", msg);
}

/**
 * Say so when the save holds a game in progress.
 *
 * No longer a warning about a broken edit -- gamefile.cpp keeps the two copies
 * in step -- but still worth stating, because it explains why writing touches
 * more of the file than the character sheet suggests.
 */
static void warn_if_game_in_progress(const char *path)
{
	if (!save_has_game(path))
		return;
	fprintf(stderr,
	    "\nnote: %s holds a game in progress.\n"
	    "  The character is stored twice -- in \"hero\", which the character\n"
	    "  selection screen shows, and in \"game\", which the game loads from once\n"
	    "  you start playing. Edits are applied to both, so they will stick; the\n"
	    "  inventory and everything else in the saved game is left alone.\n",
	    path);
}





/**
 * Flavor comes from the file extension (.hsv is Hellfire), which the user can
 * override. The packed struct is identical between the two, so this only
 * affects interpretation -- class names, stat caps, spell and item tables.
 */
static HeroFlavor g_flavor_override = (HeroFlavor)-1;

static HeroFlavor flavor_of(const char *path)
{
	if (g_flavor_override != (HeroFlavor)-1)
		return g_flavor_override;
	return hero_flavor_for_path(path);
}

/** Strip a leading --diablo/--hellfire from argv, setting the override. */
static int take_flavor_flags(int *argc, char **argv)
{
	int out = 0;
	for (int i = 0; i < *argc; i++) {
		if (strcmp(argv[i], "--hellfire") == 0)
			g_flavor_override = FLAVOR_HELLFIRE;
		else if (strcmp(argv[i], "--diablo") == 0)
			g_flavor_override = FLAVOR_DIABLO;
		else
			argv[out++] = argv[i];
	}
	*argc = out;
	return 0;
}

/* ------------------------------------------------------------------ */
/* list                                                                */
/* ------------------------------------------------------------------ */

static int cmd_list(int argc, char **argv)
{
	SaveEntry saves[SAVE_MAX_SLOTS];
	char dir[SAVE_PATH_MAX];
	int n;

	if (argc > 0) {
		snprintf(dir, sizeof(dir), "%s", argv[0]);
		n = save_scan_dir(dir, saves, SAVE_MAX_SLOTS);
	} else {
		n = save_scan_default(saves, SAVE_MAX_SLOTS, dir);
	}

	if (n == 0) {
		fprintf(stderr, "no readable saves found in %s\n", dir);
		return 1;
	}

	printf("%s\n", dir);
	printf("slot  %-20s %-9s %-9s %-7s %s\n", "name", "game", "class", "level", "gold");
	for (int i = 0; i < n; i++) {
		printf("%4d  ", saves[i].slot);
		format_brief(&saves[i].hero, saves[i].flavor, stdout);
	}

	/* Two saves sharing a name makes one of them unreachable in game. */
	for (int i = 0; i < n; i++)
		for (int j = i + 1; j < n; j++)
			if (strcasecmp(saves[i].name, saves[j].name) == 0)
				fprintf(stderr, "warning: two saves are both named \"%s\"; the game "
				                "resolves a name to the first matching slot, so one "
				                "of them is unreachable\n",
				    saves[i].name);
	return 0;
}

/* ------------------------------------------------------------------ */
/* show / dump                                                         */
/* ------------------------------------------------------------------ */

static int read_or_die(const char *path, PkPlayerStruct *h)
{
	char err[MPQ_ERR_LEN] = { 0 };
	int multi = 0;
	if (!save_read_hero(path, h, &multi, err)) {
		fprintf(stderr, "butcher: %s\n", err);
		return 0;
	}
	if (multi)
		fprintf(stderr, "note: decoded with the multiplayer password\n");
	return 1;
}

static int cmd_show(int argc, char **argv)
{
	if (argc != 1) {
		fprintf(stderr, "usage: butcher show <save.sv>\n");
		return 2;
	}
	PkPlayerStruct h;
	if (!read_or_die(argv[0], &h))
		return 1;
	HeroFlavor f = flavor_of(argv[0]);

	format_show(&h, f, stdout);

	char err[HERO_ERR_LEN];
	if (!hero_validate(&h, f, err))
		fprintf(stderr, "\nwarning: this character is already out of range: %s\n", err);
	hero_warnings(&h, f, warn_line);
	warn_if_game_in_progress(argv[0]);
	return 0;
}

static int cmd_dump(int argc, char **argv)
{
	const char *path = NULL;
	const char *out = NULL;

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
			out = argv[++i];
		else if (path == NULL)
			path = argv[i];
		else {
			fprintf(stderr, "unexpected argument: %s\n", argv[i]);
			return 2;
		}
	}
	if (path == NULL) {
		fprintf(stderr, "usage: butcher dump <save.sv> [-o hero.bin]\n");
		return 2;
	}

	PkPlayerStruct h;
	if (!read_or_die(path, &h))
		return 1;
	format_show(&h, flavor_of(path), stdout);

	if (out != NULL) {
		FILE *f = fopen(out, "wb");
		if (f == NULL) {
			fprintf(stderr, "butcher: cannot write %s\n", out);
			return 1;
		}
		size_t n = fwrite(&h, 1, sizeof(h), f);
		int bad = (n != sizeof(h)) | (fclose(f) != 0);
		if (bad) {
			fprintf(stderr, "butcher: short write to %s\n", out);
			return 1;
		}
		printf("\nwrote %zu bytes to %s\n", sizeof(h), out);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* set                                                                 */
/* ------------------------------------------------------------------ */

static int parse_int(const char *s, int *out)
{
	char *end;
	long v = strtol(s, &end, 10);
	if (*s == '\0' || *end != '\0' || v < INT_MIN || v > INT_MAX)
		return 0;
	*out = (int)v;
	return 1;
}

static int commit(const char *path, const PkPlayerStruct *h, HeroFlavor f, int backup)
{
	char err[MPQ_ERR_LEN] = { 0 };

	if (backup) {
		char bak[SAVE_PATH_MAX];
		if (!save_backup_to(path, bak, sizeof(bak), err)) {
			fprintf(stderr, "butcher: %s\n", err);
			return 1;
		}
		printf("backed up to %s\n", bak);
	}

	SaveGameSync sync = SAVE_GAME_ABSENT;
	if (!save_commit_ex(path, h, /*backup=*/0, &sync, err)) {
		fprintf(stderr, "butcher: %s\n", err);
		return 1;
	}
	printf("saved.\n");
	if (sync == SAVE_GAME_SYNCED)
		printf("the saved game in progress was updated to match.\n");
	else if (sync == SAVE_GAME_SYNCED_ITEMS)
		printf("the saved game in progress was updated to match, inventory "
		       "included.\n");
	hero_warnings(h, f, warn_line);
	return 0;
}

static int cmd_set(int argc, char **argv)
{
	const char *path = NULL;
	int dry = 0, force = 0, backup = 1, raw = 0;

	/* Spell edits are applied after the struct is loaded. */
	struct {
		int spell;
		int level;
	} spells[MAX_SPELLS];
	int nspells = 0;

	/* Collect option/value pairs first; apply once the hero is loaded. */
	struct Pending {
		const char *opt;
		const char *val;
	} pend[64];
	int npend = 0;

	for (int i = 0; i < argc; i++) {
		const char *a = argv[i];
		if (strcmp(a, "--dry-run") == 0) {
			dry = 1;
		} else if (strcmp(a, "--force") == 0) {
			force = 1;
		} else if (strcmp(a, "--no-backup") == 0) {
			backup = 0;
		} else if (strcmp(a, "--raw") == 0) {
			raw = 1;
		} else if (strncmp(a, "--", 2) == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "%s needs a value\n", a);
				return 2;
			}
			if (npend == 64) {
				fprintf(stderr, "too many options\n");
				return 2;
			}
			pend[npend].opt = a;
			pend[npend].val = argv[++i];
			npend++;
		} else if (path == NULL) {
			path = a;
		} else {
			fprintf(stderr, "unexpected argument: %s\n", a);
			return 2;
		}
	}
	if (path == NULL) {
		fprintf(stderr, "usage: butcher set <save.sv> [--name S] [--level N] ...\n"
		                "       run `butcher` with no arguments for the full list\n");
		return 2;
	}

	PkPlayerStruct before;
	if (!read_or_die(path, &before))
		return 1;
	PkPlayerStruct h = before;
	HeroFlavor flavor = flavor_of(path);

	int want_gold = -1;
	int gold_requested = 0;

	for (int i = 0; i < npend; i++) {
		const char *o = pend[i].opt;
		const char *v = pend[i].val;
		int n = 0;

#define NEEDINT()                                                    \
	do {                                                             \
		if (!parse_int(v, &n)) {                                     \
			fprintf(stderr, "%s: \"%s\" is not a number\n", o, v);   \
			return 2;                                                \
		}                                                            \
	} while (0)

		if (strcmp(o, "--name") == 0) {
			char err[HERO_ERR_LEN];
			if (!hero_name_valid(v, err)) {
				fprintf(stderr, "--name: %s\n", err);
				return 2;
			}
			char other[256];
			if (save_name_collides(path, v, other, sizeof(other))) {
				fprintf(stderr, "--name: %s already uses the name \"%s\". The game "
				                "resolves a name to the first matching save slot, so "
				                "one character would become unreachable.%s\n",
				    other, v, force ? " Proceeding anyway (--force)." : "");
				if (!force)
					return 2;
			}
			memset(h.pName, 0, PLR_NAME_LEN);
			strncpy(h.pName, v, PLR_NAME_LEN - 1);
		} else if (strcmp(o, "--class") == 0) {
			NEEDINT();
			h.pClass = (char)n;
		} else if (strcmp(o, "--level") == 0) {
			NEEDINT();
			/*
			 * A real level-up, not just the number: NextPlrLevel grants stat
			 * points, life and mana per level, and a character raised without
			 * them is one the game will never make whole. Lowering is left as
			 * a plain write -- the game does not take levels back, but nothing
			 * stops a save holding a lower one.
			 */
			if (n > h.pLevel) {
				char lerr[HERO_ERR_LEN];
				if (!hero_level_up(&h, flavor, n, lerr)) {
					fprintf(stderr, "--level: %s\n", lerr);
					return 2;
				}
			} else {
				h.pLevel = (char)n;
			}
		} else if (strcmp(o, "--exp") == 0) {
			NEEDINT();
			h.pExperience = n;
		} else if (strcmp(o, "--statpts") == 0) {
			NEEDINT();
			h.pStatPts = (BYTE)n;
		} else if (strcmp(o, "--str") == 0) {
			NEEDINT();
			h.pBaseStr = (BYTE)n;
		} else if (strcmp(o, "--mag") == 0) {
			NEEDINT();
			h.pBaseMag = (BYTE)n;
		} else if (strcmp(o, "--dex") == 0) {
			NEEDINT();
			h.pBaseDex = (BYTE)n;
		} else if (strcmp(o, "--vit") == 0) {
			NEEDINT();
			h.pBaseVit = (BYTE)n;
		} else if (strcmp(o, "--hp") == 0) {
			NEEDINT();
			h.pHPBase = raw ? n : HERO_FROM_WHOLE(n);
		} else if (strcmp(o, "--maxhp") == 0) {
			NEEDINT();
			h.pMaxHPBase = raw ? n : HERO_FROM_WHOLE(n);
		} else if (strcmp(o, "--mana") == 0) {
			NEEDINT();
			h.pManaBase = raw ? n : HERO_FROM_WHOLE(n);
		} else if (strcmp(o, "--maxmana") == 0) {
			NEEDINT();
			h.pMaxManaBase = raw ? n : HERO_FROM_WHOLE(n);
		} else if (strcmp(o, "--dlvl") == 0) {
			NEEDINT();
			h.plrlevel = (BYTE)n;
		} else if (strcmp(o, "--gold") == 0) {
			NEEDINT();
			want_gold = n;
			gold_requested = 1;
		} else if (strcmp(o, "--spell") == 0) {
			const char *eq = strchr(v, '=');
			if (eq == NULL) {
				fprintf(stderr, "--spell wants NAME=LEVEL, got \"%s\"\n", v);
				return 2;
			}
			char sname[64];
			size_t l = (size_t)(eq - v);
			if (l >= sizeof(sname))
				l = sizeof(sname) - 1;
			memcpy(sname, v, l);
			sname[l] = '\0';

			/*
			 * A bare number is accepted as a raw id. Names only resolve for
			 * spells this game defines, so a save that somehow holds a foreign
			 * id could not otherwise be repaired.
			 */
			int sid;
			if (!parse_int(sname, &sid)) {
				sid = hero_find_spell(flavor, sname);
				if (sid < 0) {
					fprintf(stderr, "--spell: unknown %s spell \"%s\"\n",
					    hero_flavor_name(flavor), sname);
					return 2;
				}
			}
			int lvl;
			if (!parse_int(eq + 1, &lvl)) {
				fprintf(stderr, "--spell: level must be a number\n");
				return 2;
			}
			if (nspells < (int)(sizeof(spells) / sizeof(spells[0]))) {
				spells[nspells].spell = sid;
				spells[nspells].level = lvl;
				nspells++;
			}
		} else {
			fprintf(stderr, "unknown option %s\n", o);
			return 2;
		}
#undef NEEDINT
	}

	for (int i = 0; i < nspells; i++) {
		char serr[HERO_ERR_LEN];
		if (!hero_set_spell_level(&h, flavor, spells[i].spell, spells[i].level,
		        serr)) {
			fprintf(stderr, "--spell: %s\n", serr);
			return 2;
		}
	}

	if (gold_requested) {
		char err[HERO_ERR_LEN];
		if (!hero_set_gold(&h, flavor, want_gold, err)) {
			fprintf(stderr, "--gold: %s\n", err);
			return 2;
		}
	}

	/* Report what changed before doing anything irreversible. */
	int changes = format_diff(&before, &h, flavor, stdout);
	if (changes == 0) {
		printf("no changes.\n");
		return 0;
	}

	DiagList checks;
	dl_init(&checks);
	hero_check(&h, flavor, &checks);
	int errors = dl_count(&checks, DIAG_ERROR);
	if (errors > 0 || dl_count(&checks, DIAG_WARNING) > 0) {
		fprintf(stderr, "\n");
		dl_report(&checks, NULL, stderr);
	}
	dl_free(&checks);

	if (errors > 0 && !force) {
		fprintf(stderr, "\nnothing was written. Pass --force to write it anyway.\n");
		return 1;
	}
	if (errors > 0)
		fprintf(stderr, "\nwriting anyway (--force).\n");

	if (dry) {
		printf("\n(dry run -- nothing written)\n");
		return 0;
	}

	printf("\n");
	return commit(path, &h, flavor, backup);
}

/* ------------------------------------------------------------------ */
/* patch                                                               */
/* ------------------------------------------------------------------ */

static int cmd_patch(int argc, char **argv)
{
	const char *path = NULL;
	const char *in = NULL;
	int backup = 1;
	int force = 0;

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
			in = argv[++i];
		else if (strcmp(argv[i], "--no-backup") == 0)
			backup = 0;
		else if (strcmp(argv[i], "--force") == 0)
			force = 1;
		else if (path == NULL)
			path = argv[i];
		else {
			fprintf(stderr, "unexpected argument: %s\n", argv[i]);
			return 2;
		}
	}
	if (path == NULL || in == NULL) {
		fprintf(stderr, "usage: butcher patch <save.sv> -i hero.bin "
		                "[--no-backup] [--force]\n");
		return 2;
	}

	PkPlayerStruct hero;
	FILE *f = fopen(in, "rb");
	if (f == NULL) {
		fprintf(stderr, "butcher: cannot open %s\n", in);
		return 1;
	}
	size_t n = fread(&hero, 1, sizeof(hero), f);
	int extra = (fgetc(f) != EOF);
	fclose(f);
	if (n != sizeof(hero) || extra) {
		fprintf(stderr, "butcher: %s must be exactly %zu bytes (a decoded "
		                "PkPlayerStruct, as written by `dump -o`)\n",
		    in, sizeof(hero));
		return 1;
	}

	char err[MPQ_ERR_LEN] = { 0 };
	PkPlayerStruct existing;
	int multi = 0;
	if (!save_read_hero(path, &existing, &multi, err)) {
		fprintf(stderr, "butcher: refusing to patch an archive that does not "
		                "read cleanly: %s\n",
		    err);
		return 1;
	}
	if (multi) {
		fprintf(stderr, "butcher: this archive's hero uses the multiplayer "
		                "password; writing it back as single-player would break "
		                "it. Multiplayer saves are not supported.\n");
		return 1;
	}

	HeroFlavor flavor = flavor_of(path);
	format_diff(&existing, &hero, flavor, stdout);

	char verr[HERO_ERR_LEN];
	if (!hero_validate(&hero, flavor, verr)) {
		if (!force) {
			fprintf(stderr, "butcher: %s\nPass --force to write it anyway.\n", verr);
			return 1;
		}
		fprintf(stderr, "warning: %s (writing anyway, --force)\n", verr);
	}

	return commit(path, &hero, flavor, backup);
}



/**
 * Read a whole stream. Returns malloc'd bytes with a NUL appended, and the
 * byte count in *out_len -- the length matters because the input might be
 * binary, and strlen would stop at the first zero.
 */
static char *slurp_stream(FILE *f, size_t *out_len)
{
	size_t cap = 65536, len = 0;
	char *buf = (char *)malloc(cap);
	size_t n;
	while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
		len += n;
		if (len + 1 >= cap) {
			cap *= 2;
			buf = (char *)realloc(buf, cap);
		}
	}
	if (ferror(f)) {
		free(buf);
		return NULL;
	}
	buf[len] = '\0';
	if (out_len != NULL)
		*out_len = len;
	return buf;
}

static char *slurp_stdin(char *err, size_t *out_len)
{
	char *t = slurp_stream(stdin, out_len);
	if (t == NULL)
		snprintf(err, MPQ_ERR_LEN, "error reading standard input");
	return t;
}

static char *slurp_len(const char *path, char *err, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		snprintf(err, MPQ_ERR_LEN, "cannot open %s", path);
		return NULL;
	}
	char *buf = slurp_stream(f, out_len);
	fclose(f);
	if (buf == NULL)
		snprintf(err, MPQ_ERR_LEN, "error reading %s", path);
	return buf;
}

/* ------------------------------------------------------------------ */
/* validate                                                            */
/* ------------------------------------------------------------------ */

/*
 * `validate` accepts any of the three shapes a character comes in: a JSON
 * document, a save archive, or the raw 1266-byte struct `dump -o` writes.
 *
 * Taking only JSON was a mistake. Every other command takes a save path, so
 * `validate single_0.sv` is the natural thing to type -- and it used to answer
 * "unexpected character 'M'", which is true (MPQ archives start with M) and
 * useless. What the user asked was "is this character valid", and that question
 * has an answer regardless of which container it arrived in.
 *
 * The JSON path additionally checks the document itself: unknown or misspelled
 * keys, wrong types, and values that would be truncated by the field they land
 * in. A save or a raw struct has no document to get wrong, so only the
 * character checks apply -- the output says which happened.
 */
typedef enum InputKind {
	INPUT_JSON,
	INPUT_SAVE,
	INPUT_RAW_STRUCT,
	INPUT_BINARY
} InputKind;

static InputKind sniff_input(const char *bytes, size_t len)
{
	/* 'MPQ\x1A' -- a save archive, or the game's asset archive. */
	if (len >= 4 && memcmp(bytes, "MPQ\x1A", 4) == 0)
		return INPUT_SAVE;

	/* Exactly the size of a decoded character: `dump -o` output. */
	if (len == sizeof(PkPlayerStruct))
		return INPUT_RAW_STRUCT;

	/* A NUL early on means this is not text at all. */
	size_t probe = len < 512 ? len : 512;
	for (size_t i = 0; i < probe; i++)
		if (bytes[i] == '\0')
			return INPUT_BINARY;

	return INPUT_JSON;
}

static const char *kind_label(InputKind k)
{
	switch (k) {
	case INPUT_SAVE:
		return "save archive";
	case INPUT_RAW_STRUCT:
		return "raw character struct";
	case INPUT_JSON:
		return "JSON document";
	default:
		return "unrecognized data";
	}
}

/* ------------------------------------------------------------------ */
/* inspect                                                             */
/* ------------------------------------------------------------------ */

static int cmd_inspect(int argc, char **argv)
{
	if (argc != 1) {
		fprintf(stderr, "usage: butcher inspect <save.sv>\n");
		return 2;
	}
	const char *path = argv[0];

	PkPlayerStruct h;
	if (!read_or_die(path, &h))
		return 1;

	char err[MPQ_ERR_LEN] = { 0 };
	DWORD glen = 0;
	int present = 0;
	BYTE *game = game_read(path, &glen, &present, err);
	if (!present) {
		printf("%s: no game in progress; the packed character is the only copy\n",
		    path);
		return 0;
	}
	if (game == NULL) {
		fprintf(stderr, "butcher: %s\n", err);
		return 1;
	}

	GameLoc loc;
	if (!game_locate(game, glen, &h, &loc, err)) {
		free(game);
		fprintf(stderr, "butcher: %s\n", err);
		return 1;
	}

	game_dump(game, glen, &loc, &h, stdout);
	free(game);
	return 0;
}

/* ------------------------------------------------------------------ */
/* fix                                                                 */
/* ------------------------------------------------------------------ */

static int cmd_fix(int argc, char **argv)
{
	const char *path = NULL;
	int dry = 0, backup = 1, all = 0;

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--dry-run") == 0)
			dry = 1;
		else if (strcmp(argv[i], "--no-backup") == 0)
			backup = 0;
		else if (strcmp(argv[i], "--all") == 0)
			all = 1;
		else if (path == NULL)
			path = argv[i];
		else {
			fprintf(stderr, "unexpected argument: %s\n", argv[i]);
			return 2;
		}
	}
	if (path == NULL) {
		fprintf(stderr, "usage: butcher fix <save.sv> [--all] [--dry-run] "
		                "[--no-backup]\n");
		return 2;
	}

	PkPlayerStruct h;
	if (!read_or_die(path, &h))
		return 1;
	HeroFlavor f = flavor_of(path);

	DiagList log;
	dl_init(&log);
	int n = hero_fix(&h, f, all, &log);

	if (n == 0) {
		dl_free(&log);
		/*
		 * Nothing to repair is not the same as nothing wrong: --all is what
		 * settles the warnings, so say so rather than implying a clean file.
		 */
		char err[HERO_ERR_LEN];
		if (hero_validate(&h, f, err)) {
			DiagList chk;
			dl_init(&chk);
			hero_check(&h, f, &chk);
			int w = dl_count(&chk, DIAG_WARNING);
			dl_free(&chk);
			if (w > 0 && !all)
				printf("%s: nothing to repair; %d warning%s left alone "
				       "(--all settles those too)\n",
				    path, w, w == 1 ? "" : "s");
			else
				printf("%s: nothing to repair\n", path);
		} else {
			printf("%s: nothing could be repaired automatically\n", path);
			fprintf(stderr, "butcher: %s\n", err);
			return 1;
		}
		return 0;
	}

	dl_report(&log, path, stdout);
	dl_free(&log);
	printf("\n%d change%s\n", n, n == 1 ? "" : "s");

	/* Never write something that still would not load. */
	char err[HERO_ERR_LEN];
	if (!hero_validate(&h, f, err)) {
		fprintf(stderr, "\nbutcher: still invalid after repair: %s\n", err);
		fprintf(stderr, "the save was not written.\n");
		return 1;
	}

	if (dry) {
		printf("\n--dry-run: nothing written\n");
		return 0;
	}
	return commit(path, &h, f, backup);
}

static int cmd_validate(int argc, char **argv)
{
	const char *path = NULL;
	int strict = 0, quiet = 0;

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--strict") == 0)
			strict = 1;
		else if (strcmp(argv[i], "--quiet") == 0)
			quiet = 1;
		else if (path == NULL)
			path = argv[i];
		else {
			fprintf(stderr, "unexpected argument: %s\n", argv[i]);
			return 2;
		}
	}
	if (path == NULL) {
		fprintf(stderr, "usage: butcher validate <char.json|save.sv> "
		                "[--strict] [--quiet]\n");
		return 2;
	}

	int from_stdin = (strcmp(path, "-") == 0);
	const char *label = from_stdin ? "<stdin>" : path;

	char err[MPQ_ERR_LEN] = { 0 };
	size_t len = 0;
	char *bytes = from_stdin ? slurp_stdin(err, &len) : slurp_len(path, err, &len);
	if (bytes == NULL) {
		fprintf(stderr, "butcher: %s\n", err);
		return 2;
	}

	InputKind kind = sniff_input(bytes, len);

	DiagList dl;
	dl_init(&dl);

	int checked_document = 0;
	int checked_character = 0;
	int doc_errors = 0;
	int hero_errors = 0;
	PkPlayerStruct h;
	HeroFlavor f = FLAVOR_DIABLO;

	switch (kind) {
	case INPUT_SAVE:
		if (from_stdin) {
			dl_add(&dl, DIAG_ERROR, NULL,
			    "this is a save archive, which has to be read from a file rather "
			    "than a pipe -- pass its path instead of \"-\"");
			break;
		}
		f = flavor_of(path);
		if (!save_read_hero(path, &h, NULL, err)) {
			/* mpq_open already explains asset archives and corrupt saves. */
			dl_add(&dl, DIAG_ERROR, NULL, "%s", err);
			break;
		}
		if (g_flavor_override != (HeroFlavor)-1)
			f = g_flavor_override;
		hero_check(&h, f, &dl);
		checked_character = 1;
		break;

	case INPUT_RAW_STRUCT:
		memcpy(&h, bytes, sizeof(h));
		f = flavor_of(path);
		if (g_flavor_override != (HeroFlavor)-1)
			f = g_flavor_override;
		hero_check(&h, f, &dl);
		checked_character = 1;
		break;

	case INPUT_BINARY:
		dl_add(&dl, DIAG_ERROR, NULL,
		    "this is not text, and not a save archive or a %zu-byte character "
		    "struct either. Expected a JSON document from `export`, a save file, "
		    "or a struct from `dump -o`.",
		    sizeof(PkPlayerStruct));
		break;

	case INPUT_JSON:
		checked_document = 1;
		charjson_check(bytes, &dl);
		doc_errors = dl_count(&dl, DIAG_ERROR);
		{
			char jerr[JSON_ERR_LEN];
			if (charjson_read(bytes, &h, &f, jerr)) {
				if (g_flavor_override != (HeroFlavor)-1)
					f = g_flavor_override;
				int before = dl_count(&dl, DIAG_ERROR);
				hero_check(&h, f, &dl);
				hero_errors = dl_count(&dl, DIAG_ERROR) - before;
				checked_character = 1;
			} else if (doc_errors == 0) {
				dl_add(&dl, DIAG_ERROR, NULL, "%s", jerr);
			}
		}
		break;
	}
	free(bytes);

	int errors = dl_count(&dl, DIAG_ERROR);
	int warnings = dl_count(&dl, DIAG_WARNING);

	if (!quiet) {
		if (errors == 0 && warnings == 0) {
			if (checked_character)
				printf("%s: ok (%s, %s character)\n", label, kind_label(kind),
				    hero_flavor_name(f));
			else
				printf("%s: ok (%s)\n", label, kind_label(kind));
		} else {
			dl_report(&dl, label, stderr);
			/*
			 * Checking is staged: the document has to be readable before the
			 * character it describes can be examined. Say when the second stage
			 * did not run, so silence is not mistaken for approval.
			 */
			if (!checked_character && errors > 0)
				fprintf(stderr, "\nnote: the character itself was not checked%s\n",
				    checked_document
				        ? " -- fix the document errors above and run validate "
				          "again for the rest."
				        : ".");
			else if (doc_errors > 0 && hero_errors > 0)
				fprintf(stderr, "\nnote: a field reported as unknown above is "
				                "ignored on import, so some character findings "
				                "may resolve once the names are fixed.\n");
		}
	}
	dl_free(&dl);

	if (errors > 0)
		return 1;
	if (strict && warnings > 0)
		return 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* export / import                                                     */
/* ------------------------------------------------------------------ */

static int cmd_export(int argc, char **argv)
{
	const char *path = NULL;
	const char *out = NULL;

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
			out = argv[++i];
		else if (path == NULL)
			path = argv[i];
		else {
			fprintf(stderr, "unexpected argument: %s\n", argv[i]);
			return 2;
		}
	}
	if (path == NULL) {
		fprintf(stderr, "usage: butcher export <save.sv> [-o char.json]\n");
		return 2;
	}

	PkPlayerStruct h;
	if (!read_or_die(path, &h))
		return 1;

	char *text = charjson_write(&h, flavor_of(path), warn_line);

	if (out == NULL || strcmp(out, "-") == 0) {
		fputs(text, stdout);
	} else {
		FILE *f = fopen(out, "wb");
		if (f == NULL) {
			fprintf(stderr, "butcher: cannot write %s\n", out);
			free(text);
			return 1;
		}
		size_t n = strlen(text);
		int wrote_all = (fwrite(text, 1, n, f) == n);
		int closed = (fclose(f) == 0);
		int bad = !wrote_all || !closed;
		if (bad) {
			fprintf(stderr, "butcher: short write to %s\n", out);
			free(text);
			return 1;
		}
		fprintf(stderr, "wrote %s\n", out);
	}
	free(text);
	return 0;
}

static int cmd_import(int argc, char **argv)
{
	const char *path = NULL;
	const char *in = NULL;
	int backup = 1, force = 0, dry = 0;

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
			in = argv[++i];
		else if (strcmp(argv[i], "--no-backup") == 0)
			backup = 0;
		else if (strcmp(argv[i], "--force") == 0)
			force = 1;
		else if (strcmp(argv[i], "--dry-run") == 0)
			dry = 1;
		else if (path == NULL)
			path = argv[i];
		else {
			fprintf(stderr, "unexpected argument: %s\n", argv[i]);
			return 2;
		}
	}
	if (path == NULL || in == NULL) {
		fprintf(stderr, "usage: butcher import <save.sv> -i char.json "
		                "[--dry-run] [--force] [--no-backup]\n");
		return 2;
	}

	char err[MPQ_ERR_LEN] = { 0 };
	char *text = slurp_len(in, err, NULL);
	if (text == NULL) {
		fprintf(stderr, "butcher: %s\n", err);
		return 1;
	}

	/* Same document checks as `validate`, so a typo is caught here too. */
	DiagList dl;
	dl_init(&dl);
	charjson_check(text, &dl);
	if (dl_has_errors(&dl)) {
		dl_report(&dl, in, stderr);
		fprintf(stderr, "\nnothing was written. Run `butcher validate %s` "
		                "after fixing.\n",
		    in);
		dl_free(&dl);
		free(text);
		return 1;
	}
	dl_free(&dl);

	PkPlayerStruct h;
	HeroFlavor doc_flavor = FLAVOR_DIABLO;
	char jerr[JSON_ERR_LEN] = { 0 };
	if (!charjson_read(text, &h, &doc_flavor, jerr)) {
		fprintf(stderr, "butcher: %s: %s\n", in, jerr);
		free(text);
		return 1;
	}
	free(text);

	/* The save's own flavor governs; a mismatch is worth saying out loud. */
	HeroFlavor flavor = flavor_of(path);
	if (doc_flavor != flavor) {
		fprintf(stderr, "warning: %s describes a %s character but %s is a %s save; "
		                "using %s\n",
		    in, hero_flavor_name(doc_flavor), path, hero_flavor_name(flavor),
		    hero_flavor_name(flavor));
	}

	PkPlayerStruct existing;
	int multi = 0;
	if (!save_read_hero(path, &existing, &multi, err)) {
		fprintf(stderr, "butcher: refusing to write an archive that does not "
		                "read cleanly: %s\n",
		    err);
		return 1;
	}
	if (multi) {
		fprintf(stderr, "butcher: this archive's hero uses the multiplayer "
		                "password; writing it back as single-player would break "
		                "it. Multiplayer saves are not supported.\n");
		return 1;
	}

	if (format_diff(&existing, &h, flavor, stdout) == 0)
		printf("no changes.\n");

	DiagList checks;
	dl_init(&checks);
	hero_check(&h, flavor, &checks);
	int errors = dl_count(&checks, DIAG_ERROR);
	if (errors > 0 || dl_count(&checks, DIAG_WARNING) > 0) {
		fprintf(stderr, "\n");
		dl_report(&checks, in, stderr);
	}
	dl_free(&checks);

	if (errors > 0 && !force) {
		fprintf(stderr, "\nnothing was written. Pass --force to write it anyway.\n");
		return 1;
	}
	if (errors > 0)
		fprintf(stderr, "\nwriting anyway (--force).\n");

	if (dry) {
		printf("\n(dry run -- nothing written)\n");
		return 0;
	}

	printf("\n");
	return commit(path, &h, flavor, backup);
}

/* ------------------------------------------------------------------ */

static int usage(void)
{
	fprintf(stderr,
	    "butcher -- Diablo and Hellfire character editor\n"
	    "\n"
	    "  butcher                     browse saves in the current directory\n"
	    "  butcher --saves             browse the game's own save folder\n"
	    "  butcher <save>              open one character in the terminal UI\n"
	    "  butcher <dir>               browse a particular directory\n"
	    "  butcher <save> --render     draw one frame and exit, for a pipe\n"
	    "\n"
	    "Everything below is the command-line interface; naming a subcommand\n"
	    "selects it. Use --tui to force the interface on a file named like one.\n"
	    "\n"
	    "  butcher list [dir]\n"
	    "        Summarize single_0.sv .. single_9.sv in a directory.\n"
	    "\n"
	    "  butcher show <save.sv>\n"
	    "        Full character sheet.\n"
	    "\n"
	    "  butcher set  <save.sv> [options]\n"
	    "        --name S      --class N     --level N     --exp N\n"
	    "        --statpts N   --str N       --mag N       --dex N\n"
	    "        --vit N       --hp N        --maxhp N     --mana N\n"
	    "        --maxmana N   --gold N      --dlvl N\n"
	    "        --spell NAME=LEVEL          (repeatable; level 0 forgets it.\n"
"                                    NAME may also be a numeric id, which\n"
"                                    is how a foreign id gets cleared)\n"
	    "        --dry-run     show the diff and stop\n"
	    "        --force       write even if a value is out of range\n"
	    "        --raw         hp/mana values are raw fixed point, not whole\n"
	    "        --no-backup   skip the <save.sv>.bak copy\n"
	    "\n"
	    "  --hellfire / --diablo   override the format guessed from the\n"
	    "        extension (.hsv is Hellfire, .sv is Diablo). Applies to any\n"
	    "        command, and to the terminal UI.\n"
	    "\n"
	    "  butcher validate <file> [--strict] [--quiet]\n"
	    "        Check a character without changing anything. Accepts a JSON\n"
	    "        document, a save file, or a raw struct from dump -o. Exit 0 if\n"
	    "        clean, 1 on errors (or on warnings, with --strict). Reads\n"
	    "        standard input when given \"-\".\n"
	    "\n"
	    "  butcher fix <save.sv> [--all] [--dry-run] [--no-backup]\n"
	    "        Repair whatever validate reports, where the right answer is\n"
	    "        unambiguous: values are clamped into the range the game\n"
	    "        accepts, and state it cannot represent is cleared. Lists every\n"
	    "        change. --all also settles the warnings -- the cached gold\n"
	    "        total, and spell state the game would correct on load.\n"
	    "        Refuses to write if the result would still not load.\n"
	    "\n"
	    "  butcher inspect <save.sv>\n"
	    "        Show both copies of the character side by side -- the packed\n"
	    "        one the selection screen reads, and the one inside a saved\n"
	    "        game that the game loads from. Use it to check whether an edit\n"
	    "        reached both.\n"
	    "\n"
	    "  butcher export <save.sv> [-o char.json]\n"
	    "  butcher import <save.sv> -i char.json [--dry-run] [--force]\n"
	    "        Round-trip the whole character as JSON. Lossless: edit any\n"
	    "        field in a text editor and import it back. -o - writes to\n"
	    "        stdout, for piping into jq.\n"
	    "\n"
	    "  butcher dump  <save.sv> [-o hero.bin]\n"
	    "  butcher patch <save.sv> -i hero.bin [--no-backup] [--force]\n"
	    "        Raw 1266-byte struct, for anything JSON does not expose.\n"
	    "\n"
	    "Life and mana are whole points unless --raw. Gold is distributed\n"
	    "across inventory stacks, because the game recomputes the cached total\n"
	    "from them. Writes are verified before replacing the original.\n");
	return 2;
}

/* Entry point when a subcommand is given; see butcher.cpp for dispatch. */
int cli_main(int argc, char **argv)
{
	/*
	 * Diffs go to stdout and warnings to stderr. Without this, stdout is
	 * block-buffered whenever output is piped or captured, and the warnings
	 * surface before the diff they refer to.
	 */
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (argc < 2)
		return usage();

	/* --diablo / --hellfire may appear anywhere; take them out first. */
	int rest = argc - 2;
	take_flavor_flags(&rest, argv + 2);

	if (strcmp(argv[1], "list") == 0)
		return cmd_list(rest, argv + 2);
	if (strcmp(argv[1], "show") == 0)
		return cmd_show(rest, argv + 2);
	if (strcmp(argv[1], "set") == 0)
		return cmd_set(rest, argv + 2);
	if (strcmp(argv[1], "validate") == 0)
		return cmd_validate(rest, argv + 2);
	if (strcmp(argv[1], "fix") == 0)
		return cmd_fix(rest, argv + 2);
	if (strcmp(argv[1], "inspect") == 0)
		return cmd_inspect(rest, argv + 2);
	if (strcmp(argv[1], "export") == 0)
		return cmd_export(rest, argv + 2);
	if (strcmp(argv[1], "import") == 0)
		return cmd_import(rest, argv + 2);
	if (strcmp(argv[1], "dump") == 0)
		return cmd_dump(rest, argv + 2);
	if (strcmp(argv[1], "patch") == 0)
		return cmd_patch(rest, argv + 2);
	return usage();
}
