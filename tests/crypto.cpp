/**
 * @file crypto_check.cpp
 *
 * Phase 1 gate for the character editor (docs/butcher_plan.md).
 *
 * Proves that Devilution's save-file crypto -- Source/sha.cpp and
 * Source/codec.cpp, compiled unmodified -- behaves identically when built
 * for a non-Windows host. Everything downstream (MPQ read, MPQ write, the
 * editor proper) is worthless if this is wrong, so it is checked first and
 * in isolation.
 *
 * WHAT THIS DOES NOT PROVE
 * ------------------------
 * No real .sv file is involved. These checks establish internal consistency
 * and agreement with the published msvcrt PRNG; they cannot establish that
 * the pipeline agrees with a save written by the retail game. That requires
 * running check_real_save() below against an actual `hero` blob. Until
 * someone does that, Phase 1 is "self-consistent", not "verified".
 */
#include "../src/compat/shim.h"

#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Compile-time layout checks                                          */
/* ------------------------------------------------------------------ */

/* CT_ASSERT comes from compat/shim.h. */
CT_ASSERT(sizeof(PkItemStruct) == 19, PkItemStruct_is_19_bytes);
CT_ASSERT(sizeof(PkPlayerStruct) == 1266, PkPlayerStruct_is_1266_bytes);

CT_ASSERT(PLR_NAME_LEN == 32, name_len_32);
CT_ASSERT(NUM_INVLOC == 7, invloc_7);
CT_ASSERT(NUM_INV_GRID_ELEM == 40, invgrid_40);
CT_ASSERT(MAXBELTITEMS == 8, belt_8);
CT_ASSERT(MAX_SPELLS == 37, spells_37);

/* Every field the editor reads or writes, pinned by offset. */
CT_ASSERT(offsetof(PkPlayerStruct, archiveTime) == 0, off_archiveTime);
CT_ASSERT(offsetof(PkPlayerStruct, plrlevel) == 11, off_plrlevel);
CT_ASSERT(offsetof(PkPlayerStruct, pName) == 16, off_pName);
CT_ASSERT(offsetof(PkPlayerStruct, pClass) == 48, off_pClass);
CT_ASSERT(offsetof(PkPlayerStruct, pBaseStr) == 49, off_pBaseStr);
CT_ASSERT(offsetof(PkPlayerStruct, pBaseMag) == 50, off_pBaseMag);
CT_ASSERT(offsetof(PkPlayerStruct, pBaseDex) == 51, off_pBaseDex);
CT_ASSERT(offsetof(PkPlayerStruct, pBaseVit) == 52, off_pBaseVit);
CT_ASSERT(offsetof(PkPlayerStruct, pLevel) == 53, off_pLevel);
CT_ASSERT(offsetof(PkPlayerStruct, pStatPts) == 54, off_pStatPts);
CT_ASSERT(offsetof(PkPlayerStruct, pExperience) == 55, off_pExperience);
CT_ASSERT(offsetof(PkPlayerStruct, pGold) == 59, off_pGold);
CT_ASSERT(offsetof(PkPlayerStruct, pHPBase) == 63, off_pHPBase);
CT_ASSERT(offsetof(PkPlayerStruct, pMaxHPBase) == 67, off_pMaxHPBase);
CT_ASSERT(offsetof(PkPlayerStruct, pManaBase) == 71, off_pManaBase);
CT_ASSERT(offsetof(PkPlayerStruct, pMaxManaBase) == 75, off_pMaxManaBase);
CT_ASSERT(offsetof(PkPlayerStruct, pSplLvl) == 79, off_pSplLvl);
CT_ASSERT(offsetof(PkPlayerStruct, pMemSpells) == 116, off_pMemSpells);
CT_ASSERT(offsetof(PkPlayerStruct, InvBody) == 124, off_InvBody);
CT_ASSERT(offsetof(PkPlayerStruct, InvList) == 257, off_InvList);
CT_ASSERT(offsetof(PkPlayerStruct, InvGrid) == 1017, off_InvGrid);
CT_ASSERT(offsetof(PkPlayerStruct, _pNumInv) == 1057, off_pNumInv);
CT_ASSERT(offsetof(PkPlayerStruct, SpdList) == 1058, off_SpdList);
CT_ASSERT(offsetof(PkPlayerStruct, pDiabloKillLevel) == 1234, off_pDiabloKillLevel);

/* ------------------------------------------------------------------ */
/* Tiny harness                                                        */
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

/** FNV-1a, for pinning byte sequences without pulling in a hash library. */
static uint64_t fnv1a(const void *p, size_t n)
{
	const uint8_t *b = (const uint8_t *)p;
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < n; i++) {
		h ^= b[i];
		h *= 1099511628211ull;
	}
	return h;
}

#define PASSWORD_SINGLE "xrgyrkj1"
#define PASSWORD_MULTI "szqnlsk1"

/* ------------------------------------------------------------------ */
/* 1. msvcrt PRNG                                                      */
/* ------------------------------------------------------------------ */

/*
 * The single external reference point available without a Windows host.
 * This sequence -- srand(1) then ten rand() calls -- is the long-published
 * output of the Microsoft C runtime, reproduced in countless references.
 * If the shim's LCG matches it, the key material codec_init_key() derives
 * is the same material MSVC and MinGW builds derive.
 */
static void check_msvc_rand(void)
{
	static const int expect[10] = {
		41, 18467, 6334, 26500, 19169, 15724, 11478, 29358, 26962, 24464
	};
	int match = 1;

	section("1. msvcrt PRNG");

	srand(1);
	for (int i = 0; i < 10; i++) {
		int got = rand();
		if (got != expect[i]) {
			printf("        index %d: expected %d, got %d\n", i, expect[i], got);
			match = 0;
		}
	}
	ok(match, "srand(1) reproduces the published msvcrt sequence");

	/* Every value must be in [0, RAND_MAX] with RAND_MAX == 0x7fff. */
	int in_range = 1;
	srand(0x7058);
	for (int i = 0; i < 4096; i++) {
		int v = rand();
		if (v < 0 || v > 0x7fff)
			in_range = 0;
	}
	ok(in_range, "output stays within msvcrt's 15-bit RAND_MAX");

	/* codec_init_key depends on this exact seed; pin its first outputs. */
	srand(0x7058);
	int a = rand(), b = rand(), c = rand();
	printf("        srand(0x7058) -> %d, %d, %d\n", a, b, c);
	ok(a == 28420 && b == 31343 && c == 25337,
	    "srand(0x7058) prefix is stable");
}

/* ------------------------------------------------------------------ */
/* 2. X-SHA-1                                                          */
/* ------------------------------------------------------------------ */

/*
 * Source/sha.cpp is not SHA-1: the message schedule omits the rotate, input
 * is never padded, and the digest is emitted as native-endian words. It also
 * carries a live compiler hazard -- MSVC global optimizations turn
 * SHA1CircularShift(5, A) into `ror edx, 0x1b`, changing the digest and
 * producing saves incompatible with vanilla (see the #pragma optimize guard
 * in that file).
 *
 * The golden values below were produced by this build. They cannot prove
 * this build is right; they exist so that a compiler, flag, or refactor that
 * silently changes the digest is caught immediately.
 */
static void check_xsha1(void)
{
	char block[64];
	char digest[SHA1HashSize];

	section("2. X-SHA-1 characterization");

	/* All-zero block from a fresh context. */
	memset(block, 0, sizeof(block));
	SHA1Reset(0);
	SHA1Calculate(0, block, digest);
	SHA1Clear();
	uint64_t h_zero = fnv1a(digest, sizeof(digest));
	printf("        zero-block digest fnv1a = 0x%016llx\n",
	    (unsigned long long)h_zero);
	ok(h_zero == 0x476b5093bf48e8f2ull, "zero-block digest unchanged");

	/* Counting pattern. */
	for (int i = 0; i < 64; i++)
		block[i] = (char)i;
	SHA1Reset(0);
	SHA1Calculate(0, block, digest);
	SHA1Clear();
	uint64_t h_seq = fnv1a(digest, sizeof(digest));
	printf("        seq-block  digest fnv1a = 0x%016llx\n",
	    (unsigned long long)h_seq);
	ok(h_seq == 0x57406383f6980be2ull, "counting-block digest unchanged");

	/*
	 * Real SHA-1 of a 64-byte zero block starts 0xC8D7D7DF. If the digest
	 * below ever matches a standard implementation, someone has "fixed"
	 * sha.cpp and every save written by this tool will be unreadable.
	 */
	DWORD first;
	memcpy(&first, digest, sizeof(first));
	ok(first != 0xC8D7D7DFu, "digest is X-SHA-1, not standard SHA-1");

	/* Chaining: two calls must not equal one call twice from reset. */
	SHA1Reset(0);
	SHA1Calculate(0, block, NULL);
	SHA1Calculate(0, block, digest);
	SHA1Clear();
	uint64_t h_twice = fnv1a(digest, sizeof(digest));
	SHA1Reset(0);
	SHA1Calculate(0, block, digest);
	SHA1Clear();
	ok(h_twice != fnv1a(digest, sizeof(digest)), "context chains across calls");
}

/* ------------------------------------------------------------------ */
/* 3. codec_get_encoded_len                                            */
/* ------------------------------------------------------------------ */

static void check_encoded_len(void)
{
	section("3. codec_get_encoded_len");

	ok(codec_get_encoded_len(1266) == 1288, "1266 -> 1288 (the hero blob)");
	ok(codec_get_encoded_len(sizeof(PkPlayerStruct)) == 1288,
	    "sizeof(PkPlayerStruct) -> 1288");
	ok(codec_get_encoded_len(0) == 8, "0 -> 8 (signature only)");
	ok(codec_get_encoded_len(1) == 72, "1 -> 72");
	ok(codec_get_encoded_len(63) == 72, "63 -> 72");
	ok(codec_get_encoded_len(64) == 72, "64 -> 72 (exact block, no pad)");
	ok(codec_get_encoded_len(65) == 136, "65 -> 136");
}

/* ------------------------------------------------------------------ */
/* 4. Round-trip                                                       */
/* ------------------------------------------------------------------ */

/** Fill a buffer with a deterministic, non-repeating-ish pattern. */
static void fill_pattern(BYTE *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		p[i] = (BYTE)((i * 31u + 7u) & 0xFF);
}

static void check_roundtrip(void)
{
	const DWORD plain_len = (DWORD)sizeof(PkPlayerStruct);
	const DWORD enc_len = codec_get_encoded_len(plain_len);

	BYTE original[1266];
	BYTE buf[1288];

	section("4. codec round-trip");

	fill_pattern(original, sizeof(original));

	memset(buf, 0, sizeof(buf));
	memcpy(buf, original, plain_len);
	codec_encode(buf, plain_len, enc_len, PASSWORD_SINGLE);

	ok(memcmp(buf, original, plain_len) != 0, "ciphertext differs from plaintext");

	uint64_t cipher_hash = fnv1a(buf, enc_len);
	printf("        ciphertext fnv1a = 0x%016llx\n",
	    (unsigned long long)cipher_hash);
	ok(cipher_hash == 0xd563222a1b3f9353ull, "ciphertext unchanged");

	int decoded = codec_decode(buf, enc_len, PASSWORD_SINGLE);
	ok(decoded == (int)plain_len, "decode returns exactly 1266");
	ok(memcmp(buf, original, plain_len) == 0, "plaintext recovered byte-for-byte");

	/* Determinism: the keystream is fixed per password, so encoding twice
	 * must produce identical output. */
	BYTE again[1288];
	memset(again, 0, sizeof(again));
	memcpy(again, original, plain_len);
	codec_encode(again, plain_len, enc_len, PASSWORD_SINGLE);
	ok(fnv1a(again, enc_len) == cipher_hash, "encoding is deterministic");

	/* A different password must produce different bytes. */
	BYTE multi[1288];
	memset(multi, 0, sizeof(multi));
	memcpy(multi, original, plain_len);
	codec_encode(multi, plain_len, enc_len, PASSWORD_MULTI);
	ok(fnv1a(multi, enc_len) != cipher_hash, "password changes the keystream");
}

/* ------------------------------------------------------------------ */
/* 5. Round-trip through a populated PkPlayerStruct                    */
/* ------------------------------------------------------------------ */

static void check_struct_roundtrip(void)
{
	PkPlayerStruct hero;
	BYTE buf[1288];
	const DWORD plain_len = (DWORD)sizeof(hero);
	const DWORD enc_len = codec_get_encoded_len(plain_len);

	section("5. PkPlayerStruct round-trip");

	memset(&hero, 0, sizeof(hero));
	strcpy(hero.pName, "Aidan");
	hero.pClass = PC_WARRIOR;
	hero.pLevel = 12;
	hero.pBaseStr = 45;
	hero.pBaseMag = 12;
	hero.pBaseDex = 28;
	hero.pBaseVit = 33;
	hero.pStatPts = 5;
	hero.pExperience = 45000;
	hero.pGold = 12345;
	hero.pHPBase = 60 << 6; /* fixed point: low 6 bits fractional */
	hero.pMaxHPBase = 60 << 6;
	hero.pManaBase = 20 << 6;
	hero.pMaxManaBase = 20 << 6;
	hero.plrlevel = 4;
	hero.pMemSpells = 0;
	for (int i = 0; i < NUM_INVLOC; i++)
		hero.InvBody[i].idx = 0xFFFF;
	for (int i = 0; i < NUM_INV_GRID_ELEM; i++)
		hero.InvList[i].idx = 0xFFFF;
	for (int i = 0; i < MAXBELTITEMS; i++)
		hero.SpdList[i].idx = 0xFFFF;

	memset(buf, 0, sizeof(buf));
	memcpy(buf, &hero, plain_len);
	codec_encode(buf, plain_len, enc_len, PASSWORD_SINGLE);
	int decoded = codec_decode(buf, enc_len, PASSWORD_SINGLE);

	ok(decoded == (int)plain_len, "decode returns 1266");

	PkPlayerStruct back;
	memcpy(&back, buf, sizeof(back));

	ok(strcmp(back.pName, "Aidan") == 0, "name survives");
	ok(back.pLevel == 12, "level survives");
	ok(back.pBaseStr == 45 && back.pBaseMag == 12 && back.pBaseDex == 28
	        && back.pBaseVit == 33,
	    "base stats survive");
	ok(back.pExperience == 45000, "experience survives");
	ok(back.pGold == 12345, "gold survives");
	ok(back.pHPBase == (60 << 6), "fixed-point HP survives");
	ok(memcmp(&back, &hero, sizeof(hero)) == 0, "whole struct is identical");
}

/* ------------------------------------------------------------------ */
/* 6. Rejection paths                                                  */
/* ------------------------------------------------------------------ */

/*
 * The editor distinguishes "corrupt save" from "wrong build" by trusting
 * codec_decode's failure signal, so its failure modes need to be real.
 */
static void check_rejections(void)
{
	const DWORD plain_len = (DWORD)sizeof(PkPlayerStruct);
	const DWORD enc_len = codec_get_encoded_len(plain_len);
	BYTE original[1288];
	BYTE buf[1288];

	section("6. rejection paths");

	memset(original, 0, sizeof(original));
	fill_pattern(original, plain_len);
	codec_encode(original, plain_len, enc_len, PASSWORD_SINGLE);

	memcpy(buf, original, enc_len);
	ok(codec_decode(buf, enc_len, PASSWORD_MULTI) == 0, "wrong password rejected");

	memcpy(buf, original, enc_len);
	buf[100] ^= 0x01;
	ok(codec_decode(buf, enc_len, PASSWORD_SINGLE) == 0, "flipped body bit rejected");

	memcpy(buf, original, enc_len);
	buf[enc_len - 8] ^= 0x01; /* CodecSignature::checksum */
	ok(codec_decode(buf, enc_len, PASSWORD_SINGLE) == 0, "corrupt checksum rejected");

	memcpy(buf, original, enc_len);
	buf[enc_len - 4] = 1; /* CodecSignature::error */
	ok(codec_decode(buf, enc_len, PASSWORD_SINGLE) == 0, "error flag rejected");

	memcpy(buf, original, enc_len);
	ok(codec_decode(buf, enc_len - 1, PASSWORD_SINGLE) == 0,
	    "non-multiple-of-64 length rejected");

	memcpy(buf, original, enc_len);
	ok(codec_decode(buf, 8, PASSWORD_SINGLE) == 0, "signature-only input rejected");
	ok(codec_decode(buf, 0, PASSWORD_SINGLE) == 0, "empty input rejected");
}

/* ------------------------------------------------------------------ */
/* 7. Real save (opt-in)                                               */
/* ------------------------------------------------------------------ */

/*
 * The check that actually closes Phase 1. Extract the `hero` file from a
 * real single_N.sv with an external MPQ tool (smpq, StormLib's MPQEditor)
 * and point BUTCHER_HERO_BLOB at it:
 *
 *     smpq -x single_0.sv hero
 *     BUTCHER_HERO_BLOB=./hero make -C tools/butcher check
 *
 * A pass means this build agrees with the retail game. Until then the suite
 * reports SKIP and Phase 1 remains self-consistent but unverified.
 */
static void check_real_save(void)
{
	section("7. real save round-trip");

	const char *path = getenv("BUTCHER_HERO_BLOB");
	if (path == NULL || path[0] == '\0') {
		printf("  SKIP  set BUTCHER_HERO_BLOB to a `hero` blob to verify\n");
		printf("        (Phase 1 is self-consistent but unverified without it)\n");
		return;
	}

	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		ok(0, "open BUTCHER_HERO_BLOB");
		return;
	}

	BYTE buf[1288];
	BYTE original[1288];
	size_t n = fread(buf, 1, sizeof(buf), f);
	int at_eof = feof(f) || fgetc(f) == EOF;
	fclose(f);

	printf("        read %zu bytes from %s\n", n, path);
	ok(n == 1288 && at_eof, "blob is exactly 1288 bytes");
	if (n != 1288)
		return;

	memcpy(original, buf, sizeof(buf));

	int decoded = codec_decode(buf, 1288, PASSWORD_SINGLE);
	ok(decoded == 1266, "decodes to 1266 bytes with the single-player password");
	if (decoded != 1266)
		return;

	PkPlayerStruct *hero = (PkPlayerStruct *)buf;
	char name[PLR_NAME_LEN + 1];
	memcpy(name, hero->pName, PLR_NAME_LEN);
	name[PLR_NAME_LEN] = '\0';
	printf("        name=\"%s\" class=%d level=%d str=%u gold=%d\n",
	    name, hero->pClass, hero->pLevel, hero->pBaseStr, hero->pGold);

	ok(hero->pLevel >= 1 && hero->pLevel <= 50, "level is plausible");
	ok(hero->pClass >= 0 && hero->pClass <= 2, "class is plausible");

	/* Re-encoding the decoded plaintext must reproduce the file exactly. */
	BYTE reenc[1288];
	memset(reenc, 0, sizeof(reenc));
	memcpy(reenc, buf, 1266);
	codec_encode(reenc, 1266, 1288, PASSWORD_SINGLE);
	ok(memcmp(reenc, original, 1288) == 0,
	    "re-encode reproduces the original file byte-for-byte");
}

/* ------------------------------------------------------------------ */

int main(void)
{
	printf("Devilution character editor -- Phase 1 crypto gate\n");
	printf("sizeof(PkPlayerStruct) = %zu, encoded = %u\n",
	    sizeof(PkPlayerStruct), (unsigned)codec_get_encoded_len(sizeof(PkPlayerStruct)));

	check_msvc_rand();
	check_xsha1();
	check_encoded_len();
	check_roundtrip();
	check_struct_roundtrip();
	check_rejections();
	check_real_save();

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
