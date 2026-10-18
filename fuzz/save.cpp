/**
 * @file save.cpp
 *
 * Fuzz the archive reader: header and table validation, hash probing, the
 * sector offset table, and PKWare explode -- everything between a file on
 * disk and a decoded PkPlayerStruct.
 *
 * The codec's checksum stops a mutated `hero` from decoding, so this reaches
 * as far as the codec and no further. What lies past it -- the saved game and
 * the character itself -- has its own harnesses.
 */
#include "fuzz.h"

#include "../src/gamefile.h"
#include "../src/savefile.h"
#include "../src/hero.h"

#include <unistd.h>

const char *const fuzz_name = "save";

/* The reader takes a path, so the input goes through a file. One per process,
 * so parallel fuzzers do not trample each other. */
static const char *scratch_path(void)
{
	static char path[512];
	if (path[0] == '\0') {
		const char *dir = getenv("TMPDIR");
		snprintf(path, sizeof(path), "%s/butcher-fuzz-%d.sv",
		    dir != NULL ? dir : "/tmp", (int)getpid());
	}
	return path;
}

static int write_input(const uint8_t *data, size_t size)
{
	FILE *f = fopen(scratch_path(), "wb");
	if (f == NULL)
		return 0;
	int ok = fwrite(data, 1, size, f) == size;
	return fclose(f) == 0 && ok;
}

void fuzz_make_seed(std::vector<uint8_t> &out)
{
	PkPlayerStruct h;
	char err[MPQ_ERR_LEN];
	hero_create(&h, FLAVOR_HELLFIRE, 3, "Fuzz", err);
	remove(scratch_path());
	if (!save_create(scratch_path(), &h, err)) {
		fprintf(stderr, "save seed: %s\n", err);
		abort();
	}
	FILE *f = fopen(scratch_path(), "rb");
	uint8_t buf[65536];
	size_t n;
	out.clear();
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		out.insert(out.end(), buf, buf + n);
	fclose(f);
	remove(scratch_path());
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (!write_input(data, size))
		return 0;

	char err[MPQ_ERR_LEN];
	MpqArchive *a = mpq_open(scratch_path(), err);
	if (a != NULL) {
		static const char *const names[] = { "hero", "game", "perml00", "(listfile)" };
		for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			DWORD len = 0;
			BYTE *b = mpq_read_file(a, names[i], &len, err);
			free(b);
		}
		mpq_close(a);
	}

	PkPlayerStruct h;
	int multi = 0;
	if (save_read_hero(scratch_path(), &h, &multi, err)) {
		DiagList dl;
		dl_init(&dl);
		hero_check(&h, FLAVOR_HELLFIRE, &dl);
		dl_free(&dl);
	}

	DWORD glen = 0;
	int present = 0;
	free(game_read(scratch_path(), &glen, &present, err));
	return 0;
}
