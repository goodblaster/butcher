/**
 * @file json.cpp
 *
 * Fuzz the JSON side: the parser, the document checker with its key and
 * range tables, the reader, and the character checks on whatever it built.
 * This is the input most likely to be written by hand, so most likely to be
 * strange.
 */
#include "fuzz.h"

#include "../src/charjson.h"

const char *const fuzz_name = "json";

void fuzz_make_seed(std::vector<uint8_t> &out)
{
	PkPlayerStruct h;
	char err[HERO_ERR_LEN];
	hero_create(&h, FLAVOR_HELLFIRE, 4, "Seedling", err);
	hero_set_gold(&h, FLAVOR_HELLFIRE, 9000, err);
	char *text = charjson_write(&h, FLAVOR_HELLFIRE, NULL);
	out.assign((const uint8_t *)text, (const uint8_t *)text + strlen(text));
	free(text);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	/* The API takes NUL-terminated text; an embedded NUL just ends it early. */
	char *text = (char *)malloc(size + 1);
	memcpy(text, data, size);
	text[size] = '\0';

	DiagList dl;
	dl_init(&dl);
	charjson_check(text, &dl);
	dl_free(&dl);

	PkPlayerStruct h;
	HeroFlavor f = FLAVOR_DIABLO;
	char err[JSON_ERR_LEN];
	if (charjson_read(text, &h, &f, err)) {
		dl_init(&dl);
		hero_check(&h, f, &dl);
		dl_free(&dl);

		/* And back out: whatever was read must be writable. */
		free(charjson_write(&h, f, NULL));

		PkPlayerStruct fixed = h;
		dl_init(&dl);
		hero_fix(&fixed, f, 1, &dl);
		dl_free(&dl);
	}
	free(text);
	return 0;
}
