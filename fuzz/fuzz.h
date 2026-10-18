/**
 * @file fuzz.h
 *
 * The contract every harness in this directory meets.
 *
 * Each harness defines LLVMFuzzerTestOneInput, the libFuzzer entry point, so
 * `make fuzz` can link it against libFuzzer where the toolchain has one. Each
 * also builds a valid seed input in memory -- the repo ships no game data, so
 * there is no corpus to check in -- which driver.cpp uses for its own
 * mutation loop and writes out with --write-seed to start a libFuzzer run.
 */
#ifndef BUTCHER_FUZZ_H
#define BUTCHER_FUZZ_H

#include <stddef.h>
#include <stdint.h>

#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/** A well-formed input for this harness, to mutate from. */
void fuzz_make_seed(std::vector<uint8_t> &out);

/** Short name, for the driver's output and seed filenames. */
extern const char *const fuzz_name;

#endif /* BUTCHER_FUZZ_H */
