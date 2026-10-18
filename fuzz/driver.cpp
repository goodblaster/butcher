/**
 * @file driver.cpp
 *
 * A stand-in for libFuzzer on toolchains that lack it -- Apple clang among
 * them -- so the harnesses still run, under the sanitizers, on any machine.
 *
 *   fuzz_x                      mutate the built-in seed (default 20000 runs)
 *   fuzz_x --iterations N       ...N times
 *   fuzz_x --seed S             ...from a different random stream
 *   fuzz_x --write-seed DIR     write the seed to DIR, to start libFuzzer from
 *   fuzz_x FILE...              run each file once, e.g. a crash to reproduce
 *
 * The mutations are blunt -- bit flips, boundary values, truncation, splices
 * -- and deterministic for a given --seed, so a failure here reproduces
 * exactly. Coverage-guided fuzzing is `make fuzz`'s job, not this one's.
 */
#include "fuzz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t g_rng;

static uint32_t rnd(uint32_t n)
{
	/* xorshift64*: small, fast, and the same everywhere. */
	g_rng ^= g_rng >> 12;
	g_rng ^= g_rng << 25;
	g_rng ^= g_rng >> 27;
	uint64_t r = g_rng * 0x2545F4914F6CDD1Dull;
	return n == 0 ? 0 : (uint32_t)((r >> 32) % n);
}

static void mutate(std::vector<uint8_t> &d)
{
	static const uint32_t interesting[] = {
		0, 1, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF,
		0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu, 4096, 0x10068,
	};
	int rounds = 1 + (int)rnd(8);
	for (int r = 0; r < rounds; r++) {
		if (d.empty()) {
			d.push_back((uint8_t)rnd(256));
			continue;
		}
		size_t at = rnd((uint32_t)d.size());
		switch (rnd(7)) {
		case 0:
			d[at] ^= (uint8_t)(1u << rnd(8));
			break;
		case 1:
			d[at] = (uint8_t)rnd(256);
			break;
		case 2: {
			uint32_t v = interesting[rnd(sizeof(interesting) / sizeof(interesting[0]))];
			for (int b = 0; b < 4 && at + b < d.size(); b++)
				d[at + b] = (uint8_t)(v >> (8 * b));
			break;
		}
		case 3:
			d.resize(at);
			break;
		case 4:
			d.insert(d.begin() + at, (uint8_t)rnd(256));
			break;
		case 5: {
			/* Copy one run over another: the kind of damage that keeps a
			 * structure plausible enough to get past the first check. */
			size_t from = rnd((uint32_t)d.size());
			size_t len = 1 + rnd(64);
			for (size_t i = 0; i < len && from + i < d.size() && at + i < d.size(); i++)
				d[at + i] = d[from + i];
			break;
		}
		default: {
			size_t len = 1 + rnd(16);
			if (at + len > d.size())
				len = d.size() - at;
			d.erase(d.begin() + at, d.begin() + at + len);
			break;
		}
		}
	}
}

static int run_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		fprintf(stderr, "%s: cannot open %s\n", fuzz_name, path);
		return 0;
	}
	std::vector<uint8_t> d;
	uint8_t buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		d.insert(d.end(), buf, buf + n);
	fclose(f);
	LLVMFuzzerTestOneInput(d.data(), d.size());
	return 1;
}

int main(int argc, char **argv)
{
	long iterations = 20000;
	uint64_t seed = 1;
	const char *write_dir = NULL;
	int files = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc)
			iterations = atol(argv[++i]);
		else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
			seed = strtoull(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "--write-seed") == 0 && i + 1 < argc)
			write_dir = argv[++i];
		else {
			if (!run_file(argv[i]))
				return 2;
			files++;
		}
	}
	if (files > 0) {
		printf("%s: %d file(s) ran clean\n", fuzz_name, files);
		return 0;
	}

	std::vector<uint8_t> base;
	fuzz_make_seed(base);

	if (write_dir != NULL) {
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s.seed", write_dir, fuzz_name);
		FILE *f = fopen(path, "wb");
		if (f == NULL || fwrite(base.data(), 1, base.size(), f) != base.size()) {
			fprintf(stderr, "%s: cannot write %s\n", fuzz_name, path);
			return 2;
		}
		fclose(f);
		printf("%s: wrote %s (%zu bytes)\n", fuzz_name, path, base.size());
		return 0;
	}

	g_rng = seed * 0x9E3779B97F4A7C15ull + 1;

	/* The unmutated seed first: a harness that rejects its own seed is
	 * exercising nothing past the first check. */
	LLVMFuzzerTestOneInput(base.data(), base.size());

	std::vector<uint8_t> d;
	for (long i = 0; i < iterations; i++) {
		/* Mostly fresh from the seed; sometimes keep stacking damage. */
		if (d.empty() || rnd(4) != 0)
			d = base;
		mutate(d);
		LLVMFuzzerTestOneInput(d.data(), d.size());
	}
	printf("%s: %ld mutated inputs ran clean (seed %llu)\n", fuzz_name, iterations,
	    (unsigned long long)seed);
	return 0;
}
