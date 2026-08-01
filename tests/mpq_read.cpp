/**
 * @file mpq_check.cpp
 *
 * Phase 2 gate: the MPQ reader in savefile.cpp.
 *
 * No real .sv is required. The suite builds synthetic archives in the exact
 * fixed layout mpqapi.cpp writes -- header, encrypted block/hash tables,
 * PKWare-imploded 4096-byte sectors -- then reads them back. The builder here
 * is deliberately independent of the reader: it drives Encrypt() and
 * PkwareCompress() directly rather than sharing any of the reader's logic,
 * so a bug would have to occur identically in both to go unnoticed.
 *
 * What that still does not cover: agreement with an archive written by the
 * retail game. Point BUTCHER_SAVE at a real single_N.sv to check that.
 */
#include "../src/hero.h"
#include "../src/savefile.h"

#include "../third_party/devilution/Source/encrypt.h"

#include <sys/stat.h>
#include <unistd.h> /* truncate */

/* ------------------------------------------------------------------ */
/* Harness                                                             */
/* ------------------------------------------------------------------ */

static int g_pass, g_fail;

static void ok(int cond, const char *what)
{
	if (cond) {
		g_pass++;
		printf("  ok    %s\n", what);
	} else {
		g_fail++;
		printf("  FAIL  %s\n", what);
	}
}

static void section(const char *name)
{
	printf("\n%s\n", name);
}

static const char *g_tmpdir;

static const char *tmppath(const char *leaf)
{
	static char buf[1024];
	snprintf(buf, sizeof(buf), "%s/%s", g_tmpdir, leaf);
	return buf;
}

/* ------------------------------------------------------------------ */
/* Synthetic archive builder                                           */
/* ------------------------------------------------------------------ */

typedef struct Entry {
	const char *name;
	const BYTE *data;
	DWORD len;
} Entry;

/**
 * Write an archive in devilution's fixed layout.
 * Returns 0 on success.
 */
static int build_archive(const char *path, const Entry *entries, int count)
{
	_FILEHEADER hdr;
	_BLOCKENTRY *block = (_BLOCKENTRY *)calloc(MPQ_INDEX_ENTRIES, sizeof(_BLOCKENTRY));
	_HASHENTRY *hash = (_HASHENTRY *)malloc(MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY));
	BYTE *data = NULL;
	DWORD data_len = 0;
	int rc = -1;

	memset(hash, 0xFF, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY));
	InitHash();

	for (int e = 0; e < count; e++) {
		DWORD file_len = entries[e].len;
		DWORD nsectors = (file_len + MPQ_SECTOR_SIZE - 1) / MPQ_SECTOR_SIZE;
		DWORD table_bytes = (nsectors + 1) * sizeof(DWORD);

		/* Compress each sector, mirroring mpqapi_write_file_contents. */
		BYTE *body = (BYTE *)malloc(table_bytes + nsectors * MPQ_SECTOR_SIZE * 2);
		DWORD *offs = (DWORD *)body;
		DWORD pos = table_bytes;

		for (DWORD s = 0; s < nsectors; s++) {
			DWORD raw = file_len - s * MPQ_SECTOR_SIZE;
			if (raw > MPQ_SECTOR_SIZE)
				raw = MPQ_SECTOR_SIZE;

			BYTE sector[MPQ_SECTOR_SIZE];
			memcpy(sector, entries[e].data + s * MPQ_SECTOR_SIZE, raw);

			/* PkwareCompress keeps the imploded form only when smaller,
			 * so incompressible input stays literal -- exactly the case
			 * the reader detects via stored == raw. */
			int stored = PkwareCompress(sector, (int)raw);

			offs[s] = pos;
			memcpy(body + pos, sector, (size_t)stored);
			pos += (DWORD)stored;
		}
		offs[nsectors] = pos;

		DWORD block_off = MPQ_DATA_OFFSET + data_len;
		data = (BYTE *)realloc(data, data_len + pos);
		memcpy(data + data_len, body, pos);
		data_len += pos;
		free(body);

		block[e].offset = (int)block_off;
		block[e].sizealloc = (int)pos;
		block[e].sizefile = (int)file_len;
		block[e].flags = (int)(MPQ_FLAG_EXISTS | MPQ_FLAG_IMPLODE);

		DWORD idx = Hash(entries[e].name, 0) & 0x7FF;
		while (hash[idx].block != -1)
			idx = (idx + 1) & 0x7FF;
		hash[idx].hashcheck[0] = (int)Hash(entries[e].name, 1);
		hash[idx].hashcheck[1] = (int)Hash(entries[e].name, 2);
		hash[idx].lcid = 0;
		hash[idx].block = e;
	}

	long total = MPQ_DATA_OFFSET + (long)data_len;

	memset(&hdr, 0, sizeof(hdr));
	hdr.signature = (int)MPQ_SIGNATURE;
	hdr.headersize = 32;
	hdr.filesize = (int)total;
	hdr.version = 0;
	hdr.sectorsizeid = 3;
	hdr.hashoffset = MPQ_HASH_OFFSET;
	hdr.blockoffset = MPQ_BLOCK_OFFSET;
	hdr.hashcount = MPQ_INDEX_ENTRIES;
	hdr.blockcount = MPQ_INDEX_ENTRIES;

	Encrypt((DWORD *)block, MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY), Hash("(block table)", 3));
	Encrypt((DWORD *)hash, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY), Hash("(hash table)", 3));

	FILE *f = fopen(path, "wb");
	if (f == NULL)
		goto done;
	if (fwrite(&hdr, 1, sizeof(hdr), f) != sizeof(hdr))
		goto close;
	if (fwrite(block, 1, MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY), f)
	    != MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY))
		goto close;
	if (fwrite(hash, 1, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY), f)
	    != MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY))
		goto close;
	if (data_len && fwrite(data, 1, data_len, f) != data_len)
		goto close;
	rc = 0;
close:
	fclose(f);
done:
	free(block);
	free(hash);
	free(data);
	return rc;
}

/* ------------------------------------------------------------------ */

static void fill_pattern(BYTE *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		p[i] = (BYTE)((i * 31u + 7u) & 0xFF);
}

/** Encode a populated hero into the 1288-byte on-disk form. */
static void make_hero_blob(BYTE *out1288, PkPlayerStruct *plain)
{
	memset(plain, 0, sizeof(*plain));
	strcpy(plain->pName, "Deckard");
	plain->pClass = PC_SORCERER;
	plain->pLevel = 27;
	plain->pBaseStr = 20;
	plain->pBaseMag = 80;
	plain->pBaseDex = 30;
	plain->pBaseVit = 25;
	plain->pStatPts = 3;
	plain->pExperience = 1250000;
	plain->pGold = 7500;
	plain->pHPBase = 85 << 6;
	plain->pMaxHPBase = 85 << 6;
	plain->pManaBase = 190 << 6;
	plain->pMaxManaBase = 190 << 6;
	plain->plrlevel = 9;
	plain->_pNumInv = 2;
	for (int i = 0; i < NUM_INVLOC; i++)
		plain->InvBody[i].idx = 0xFFFF;
	for (int i = 0; i < NUM_INV_GRID_ELEM; i++)
		plain->InvList[i].idx = 0xFFFF;
	for (int i = 0; i < MAXBELTITEMS; i++)
		plain->SpdList[i].idx = 0xFFFF;

	/* Two gold stacks summing to the cached pGold. */
	plain->InvList[0].idx = IDI_GOLD;
	plain->InvList[0].wValue = 5000;
	plain->InvList[1].idx = IDI_GOLD;
	plain->InvList[1].wValue = 2500;

	memset(out1288, 0, 1288);
	memcpy(out1288, plain, sizeof(*plain));
	codec_encode(out1288, sizeof(*plain), 1288, SAVE_PASSWORD_SINGLE);
}

/* ------------------------------------------------------------------ */
/* 1. Happy path                                                       */
/* ------------------------------------------------------------------ */

static BYTE g_blob[1288];
static PkPlayerStruct g_plain;

static void check_roundtrip(void)
{
	section("1. read a synthetic archive");

	Entry e = { "hero", g_blob, 1288 };
	const char *path = tmppath("single_0.sv");
	ok(build_archive(path, &e, 1) == 0, "build synthetic archive");

	char err[MPQ_ERR_LEN] = { 0 };
	MpqArchive *a = mpq_open(path, err);
	ok(a != NULL, "mpq_open accepts it");
	if (a == NULL) {
		printf("        %s\n", err);
		return;
	}

	ok(mpq_has_file(a, "hero"), "finds \"hero\"");
	ok(!mpq_has_file(a, "game"), "does not find absent \"game\"");
	ok(mpq_has_file(a, "HERO"), "name lookup is case-insensitive");

	DWORD len = 0;
	BYTE *got = mpq_read_file(a, "hero", &len, err);
	ok(got != NULL, "reads \"hero\"");
	if (got != NULL) {
		ok(len == 1288, "length is 1288");
		ok(memcmp(got, g_blob, 1288) == 0, "bytes match what was written");
		free(got);
	} else {
		printf("        %s\n", err);
	}

	DWORD dummy;
	ok(mpq_read_file(a, "nope", &dummy, err) == NULL, "missing file reports an error");
	mpq_close(a);

	/* End to end through the codec. */
	PkPlayerStruct hero;
	ok(save_read_hero(path, &hero, NULL, err), "save_read_hero decodes it");
	ok(memcmp(&hero, &g_plain, sizeof(hero)) == 0, "decoded struct is identical");
	ok(strcmp(hero.pName, "Deckard") == 0, "name round-trips");
	ok(hero.pLevel == 27 && hero.pGold == 7500, "fields round-trip");
}

/* ------------------------------------------------------------------ */
/* 2. Sector handling                                                  */
/* ------------------------------------------------------------------ */

static void check_sectors(void)
{
	section("2. sector handling");

	/* Highly compressible: exercises the exploded path. */
	DWORD big_len = 10000;
	BYTE *zeros = (BYTE *)calloc(1, big_len);

	/* Incompressible: exercises the literal (stored == raw) path. */
	BYTE *noise = (BYTE *)malloc(big_len);
	uint32_t s = 12345;
	for (DWORD i = 0; i < big_len; i++) {
		s = s * 1103515245u + 12345u;
		noise[i] = (BYTE)(s >> 16);
	}

	BYTE tiny[7] = { 1, 2, 3, 4, 5, 6, 7 };
	BYTE *exact = (BYTE *)malloc(MPQ_SECTOR_SIZE);
	fill_pattern(exact, MPQ_SECTOR_SIZE);

	Entry entries[5] = {
		{ "hero", g_blob, 1288 },
		{ "zeros", zeros, big_len },
		{ "noise", noise, big_len },
		{ "tiny", tiny, sizeof(tiny) },
		{ "exact", exact, MPQ_SECTOR_SIZE },
	};

	const char *path = tmppath("sectors.sv");
	ok(build_archive(path, entries, 5) == 0, "build multi-file archive");

	char err[MPQ_ERR_LEN] = { 0 };
	MpqArchive *a = mpq_open(path, err);
	ok(a != NULL, "opens");
	if (a == NULL) {
		printf("        %s\n", err);
		free(zeros);
		free(noise);
		free(exact);
		return;
	}

	for (int i = 0; i < 5; i++) {
		DWORD len = 0;
		BYTE *got = mpq_read_file(a, entries[i].name, &len, err);
		char label[128];
		snprintf(label, sizeof(label), "\"%s\" (%u bytes, %u sectors) round-trips",
		    entries[i].name, (unsigned)entries[i].len,
		    (unsigned)((entries[i].len + MPQ_SECTOR_SIZE - 1) / MPQ_SECTOR_SIZE));
		if (got == NULL) {
			ok(0, label);
			printf("        %s\n", err);
			continue;
		}
		ok(len == entries[i].len && memcmp(got, entries[i].data, len) == 0, label);
		free(got);
	}

	mpq_close(a);
	free(zeros);
	free(noise);
	free(exact);
}

/* ------------------------------------------------------------------ */
/* 3. Rejection, and never touching the file                           */
/* ------------------------------------------------------------------ */

static long file_size(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static uint64_t file_hash(const char *p)
{
	FILE *f = fopen(p, "rb");
	if (f == NULL)
		return 0;
	uint64_t h = 1469598103934665603ull;
	int c;
	while ((c = fgetc(f)) != EOF) {
		h ^= (uint8_t)c;
		h *= 1099511628211ull;
	}
	fclose(f);
	return h;
}

/** Corrupt one byte at a given offset in a fresh copy of the good archive. */
static const char *make_corrupt(const char *leaf, long off, BYTE value)
{
	Entry e = { "hero", g_blob, 1288 };
	const char *path = tmppath(leaf);
	if (build_archive(path, &e, 1) != 0)
		return NULL;
	FILE *f = fopen(path, "r+b");
	if (f == NULL)
		return NULL;
	fseek(f, off, SEEK_SET);
	fputc(value, f);
	fclose(f);
	return path;
}

static void expect_refusal(const char *path, const char *what)
{
	if (path == NULL) {
		ok(0, what);
		return;
	}
	uint64_t before = file_hash(path);
	long size_before = file_size(path);

	char err[MPQ_ERR_LEN] = { 0 };
	MpqArchive *a = mpq_open(path, err);
	int refused = (a == NULL);
	if (a != NULL) {
		/* Header passed; the read must fail instead. */
		DWORD len;
		BYTE *b = mpq_read_file(a, "hero", &len, err);
		refused = (b == NULL);
		free(b);
		mpq_close(a);
	}
	ok(refused, what);
	if (refused)
		printf("        %s\n", err);

	ok(file_hash(path) == before && file_size(path) == size_before,
	    "  ...and the file is byte-for-byte unchanged");
}

static void check_rejections(void)
{
	section("3. rejection paths (file must never be modified)");

	expect_refusal(make_corrupt("bad_sig.sv", 0, 0x00), "bad signature refused");
	expect_refusal(make_corrupt("bad_hdrsize.sv", 4, 0x40), "bad headersize refused");
	expect_refusal(make_corrupt("bad_filesize.sv", 8, 0xFF), "filesize mismatch refused");
	expect_refusal(make_corrupt("bad_sectorid.sv", 14, 0x05), "bad sectorsizeid refused");
	expect_refusal(make_corrupt("bad_hashoff.sv", 16, 0x01), "bad hashoffset refused");
	expect_refusal(make_corrupt("bad_blockoff.sv", 20, 0x01), "bad blockoffset refused");
	/* Offset +6 lands in the 8-byte sector offset table, so this exercises
	 * table validation rather than the decompressor. */
	expect_refusal(make_corrupt("bad_sectbl.sv", MPQ_DATA_OFFSET + 6, 0x5A),
	    "corrupt sector offset table refused");

	/* Truncated file. */
	Entry e = { "hero", g_blob, 1288 };
	const char *path = tmppath("truncated.sv");
	build_archive(path, &e, 1);
	truncate(path, 5000);
	expect_refusal(path, "truncated archive refused");

	/* Not an MPQ at all. */
	const char *junk = tmppath("junk.sv");
	FILE *f = fopen(junk, "wb");
	for (int i = 0; i < 100000; i++)
		fputc(i & 0xFF, f);
	fclose(f);
	expect_refusal(junk, "non-MPQ file refused");

	/* Empty file. */
	const char *empty = tmppath("empty.sv");
	fclose(fopen(empty, "wb"));
	expect_refusal(empty, "empty file refused");

	/* Corrupt payload at the codec layer: valid MPQ, bad plaintext. */
	BYTE bad[1288];
	memcpy(bad, g_blob, sizeof(bad));
	bad[64] ^= 0xFF;
	Entry be = { "hero", bad, 1288 };
	const char *badpath = tmppath("bad_codec.sv");
	build_archive(badpath, &be, 1);
	PkPlayerStruct hero;
	char err[MPQ_ERR_LEN] = { 0 };
	ok(!save_read_hero(badpath, &hero, NULL, err), "hero failing the codec checksum is refused");
	printf("        %s\n", err);

	/*
	 * Corrupt the compressed payload itself, byte by byte, past the sector
	 * offset table. Every one must be rejected -- by the decompressor, by
	 * the length check, or failing those by the codec checksum. Which layer
	 * catches it does not matter; silently returning a wrong character does.
	 */
	int leaked = 0, checked = 0;
	for (long off = 8; off < 40; off++) {
		char leaf[64];
		snprintf(leaf, sizeof(leaf), "payload_%ld.sv", off);
		const char *p = make_corrupt(leaf, MPQ_DATA_OFFSET + off, 0x5A);
		if (p == NULL)
			continue;
		checked++;

		PkPlayerStruct got;
		char e2[MPQ_ERR_LEN] = { 0 };
		if (save_read_hero(p, &got, NULL, e2) && memcmp(&got, &g_plain, sizeof(got)) != 0) {
			printf("        offset +%ld produced a DIFFERENT character silently\n", off);
			leaked++;
		}
		remove(p);
	}
	printf("        probed %d single-byte payload corruptions\n", checked);
	ok(leaked == 0, "no corrupted payload ever yields a wrong character silently");
}

/* ------------------------------------------------------------------ */
/* 4. Real save (opt-in)                                               */
/* ------------------------------------------------------------------ */

static void check_real_save(void)
{
	section("4. real save");

	const char *path = getenv("BUTCHER_SAVE");
	if (path == NULL || path[0] == '\0') {
		printf("  SKIP  set BUTCHER_SAVE=/path/to/single_0.sv to verify against\n");
		printf("        an archive written by the game (this suite only proves the\n");
		printf("        reader agrees with a synthetic archive)\n");
		return;
	}

	char err[MPQ_ERR_LEN] = { 0 };
	MpqArchive *a = mpq_open(path, err);
	if (a == NULL) {
		ok(0, "mpq_open on a real archive");
		printf("        %s\n", err);
		return;
	}
	ok(1, "mpq_open on a real archive");

	/* Report the shape of the archive -- useful when something is off. */
	printf("        contains:");
	static const char *known[] = { "hero", "game" };
	for (int i = 0; i < 2; i++)
		if (mpq_has_file(a, known[i]))
			printf(" %s", known[i]);
	int perml = 0, perms = 0;
	for (int i = 0; i < NUMLEVELS; i++) {
		char n[16];
		snprintf(n, sizeof(n), "perml%02d", i);
		perml += mpq_has_file(a, n) ? 1 : 0;
		snprintf(n, sizeof(n), "perms%02d", i);
		perms += mpq_has_file(a, n) ? 1 : 0;
	}
	printf(" perml*=%d perms*=%d\n", perml, perms);

	DWORD len = 0;
	BYTE *raw = mpq_read_file(a, "hero", &len, err);
	mpq_close(a);
	if (raw == NULL) {
		ok(0, "reads \"hero\" out of a real archive");
		printf("        %s\n", err);
		return;
	}
	ok(1, "reads \"hero\" out of a real archive");
	ok(len == 1288, "\"hero\" is 1288 bytes");
	if (len != 1288) {
		free(raw);
		return;
	}

	BYTE original[1288];
	memcpy(original, raw, 1288);

	int decoded = codec_decode(raw, 1288, SAVE_PASSWORD_SINGLE);
	ok(decoded == (int)sizeof(PkPlayerStruct),
	    "decodes to 1266 bytes with the single-player password");
	if (decoded != (int)sizeof(PkPlayerStruct)) {
		free(raw);
		return;
	}

	PkPlayerStruct *hero = (PkPlayerStruct *)raw;
	char name[PLR_NAME_LEN + 1];
	memcpy(name, hero->pName, PLR_NAME_LEN);
	name[PLR_NAME_LEN] = '\0';
	printf("        name=\"%s\" class=%d level=%d str=%u gold=%d\n",
	    name, hero->pClass, hero->pLevel, hero->pBaseStr, hero->pGold);

	ok(name[0] != '\0', "name is non-empty");
	ok(hero->pLevel >= 1 && hero->pLevel <= 50, "level is plausible");
	/* Bound by the flavor, not hardcoded: a Hellfire Monk is class 3, and
	 * this assertion predated Hellfire support. */
	ok(hero->pClass >= 0
	        && hero->pClass < hero_num_classes(hero_flavor_for_path(path)),
	    "class is plausible for this game");

	/*
	 * The check that closes Phase 1 as well: re-encoding the decoded
	 * plaintext must reproduce the archived bytes exactly. If this passes
	 * on a save written by the retail game, the msvcrt-rand shim and the
	 * X-SHA-1 build are both correct.
	 */
	BYTE reenc[1288];
	memset(reenc, 0, sizeof(reenc));
	memcpy(reenc, raw, sizeof(PkPlayerStruct));
	codec_encode(reenc, sizeof(PkPlayerStruct), 1288, SAVE_PASSWORD_SINGLE);
	ok(memcmp(reenc, original, 1288) == 0,
	    "re-encode reproduces the archived bytes exactly");

	free(raw);
}

/* ------------------------------------------------------------------ */

int main(void)
{
	g_tmpdir = getenv("BUTCHER_TMPDIR");
	if (g_tmpdir == NULL) {
		/* Run directly rather than through `make check`: keep scratch files
		 * out of the source tree. */
		g_tmpdir = "build/test-scratch";
		mkdir(g_tmpdir, 0755);
	}

	printf("Devilution character editor -- Phase 2 MPQ reader\n");
	printf("scratch dir: %s\n", g_tmpdir);

	make_hero_blob(g_blob, &g_plain);

	check_roundtrip();
	check_sectors();
	check_rejections();
	check_real_save();

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
