# butcher -- a character editor for Diablo and Hellfire saves.
#
#   make            build butcher and the test binaries
#   make check      build and run every test
#   make install    copy butcher into PREFIX/bin (default /usr/local)
#   make uninstall
#   make clean
#
# Host-native; needs only a C++17 compiler. No Wine, no Storm.dll, no game
# assets, no CMake. Requires both submodules -- devilution for the save
# format, FTXUI for the terminal interface:
#
#   git submodule update --init
#
# Verify against a real save (strongly recommended before editing one you
# care about -- see README.md):
#
#   BUTCHER_SAVE=/path/to/single_0.sv make check

DEVILUTION := third_party/devilution
SHIM := src/compat/shim.h

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

CXX ?= c++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers

# Record each object's header dependencies so editing a header rebuilds what
# includes it. Without this, changing a signature in a header left stale
# objects behind and the failure surfaced as a confusing link error.
CXXFLAGS += -MMD -MP

# Extra instrumentation, set by the asan and fuzz targets below. Kept separate
# from CXXFLAGS so it survives a CXXFLAGS given on the command line.
SANITIZE ?=
CXXFLAGS += $(SANITIZE)

BUILD := build

# `all` is declared further down, after the rules it depends on, so make would
# otherwise take the first target it meets -- $(BUILD)/tui_editor.o -- as the
# default and a bare `make` would build one object file and stop, reporting
# success. Name the goal here instead of relying on rule order.
.DEFAULT_GOAL := all


# ---------------------------------------------------------------------------
# Files from the devilution submodule.
#
# These are compiled *unmodified*. They are a byte-accuracy reconstruction of
# the original game and must never be edited -- see README.md. The shim is
# force-included so their `#include "all.h"` finds an already-defined include
# guard and expands to nothing, which is the only way to build them off Windows
# without touching them.
#
# The warning suppressions mirror what the game's own types.h does with
# #pragma warning; these are vanilla warnings in code we cannot change.
# ---------------------------------------------------------------------------
GAME_FLAGS := -include $(SHIM) -Wno-sign-compare -Wno-char-subscripts \
	-Wno-unused-const-variable

# PKWare is Ladislav Zezula's; its warnings are equally off-limits.
PKWARE_FLAGS := -Wno-sign-compare -Wno-unused-variable -Wno-int-in-bool-context

# Under the sanitizers, one exemption for the same code. X-SHA-1 does its
# 32-bit arithmetic in signed int and relies on it wrapping -- SHA1CircularShift
# shifts past the range and the round sums overflow (sha.cpp:55). Both are
# undefined in C++17, and both are how the game computes every save's checksum.
# The file cannot be edited, and the crypto suite pins its digests at every
# optimization level, so the two arithmetic checks are off for vendored code
# only. Memory checks, and every check on butcher's own code, stay on.
ifneq ($(SANITIZE),)
VENDOR_NOSAN := -fno-sanitize=shift-base,signed-integer-overflow
GAME_FLAGS += $(VENDOR_NOSAN)
PKWARE_FLAGS += $(VENDOR_NOSAN)
endif

GAME_OBJ := \
	$(BUILD)/sha.o \
	$(BUILD)/codec.o \
	$(BUILD)/encrypt.o \
	$(BUILD)/explode.o \
	$(BUILD)/implode.o \
	$(BUILD)/spelldat.o \
	$(BUILD)/itemdat.o \
	$(BUILD)/spelldat_hf.o \
	$(BUILD)/itemdat_hf.o \
	$(BUILD)/hellfire.o

# The shared library. Both front ends link this; neither invokes the other.
LIB_OBJ := \
	$(BUILD)/shim.o \
	$(BUILD)/savefile.o \
	$(BUILD)/gamefile.o \
	$(BUILD)/saveutil.o \
	$(BUILD)/hero.o \
	$(BUILD)/format.o \
	$(BUILD)/json.o \
	$(BUILD)/charjson.o \
	$(BUILD)/diag.o \
	$(BUILD)/errbuf.o

# --- FTXUI ------------------------------------------------------------------
# Vendored as a submodule and compiled with this Makefile rather than its own
# CMake. It is plain C++17 with no library dependencies, so globbing its
# sources works; only its tests and fuzzers are excluded.
FTXUI := third_party/ftxui
#
# Take the source list from FTXUI's own CMakeLists rather than globbing.
# Globbing is wrong here: src/ftxui/component/loop.cpp exists in the tree but
# is not part of their build, and compiling it duplicates every ftxui::Loop
# symbol in app.cpp. Reading their list means the set stays correct when the
# submodule moves.
FTXUI_SRC := $(shell grep -o '^  src/ftxui/.*\.cpp$$' $(FTXUI)/CMakeLists.txt \
	2>/dev/null | sed 's|^  |$(FTXUI)/|')
FTXUI_OBJ := $(patsubst $(FTXUI)/src/%.cpp,$(BUILD)/ftxui/%.o,$(FTXUI_SRC))
FTXUI_INC := -I$(FTXUI)/include -I$(FTXUI)/src

# Checked here rather than as a prerequisite: FTXUI_SRC above is expanded while
# the makefile is read, so a missing submodule otherwise surfaces as a stray
# grep error and a link failure with no explanation. `clean` stays usable.
ifneq ($(MAKECMDGOALS),clean)
ifeq ($(wildcard $(DEVILUTION)/defs.h),)
$(error The devilution submodule is missing. Run: git submodule update --init)
endif
ifeq ($(wildcard $(FTXUI)/CMakeLists.txt),)
$(error The FTXUI submodule is missing. Run: git submodule update --init)
endif
endif

$(BUILD)/ftxui/%.o: $(FTXUI)/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(FTXUI_INC) -c -o $@ $<

$(BUILD)/tui_editor.o: tui/editor.cpp tui/editor.h $(SHIM) | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/tui_nav.o: tui/nav.cpp tui/nav.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $(FTXUI_INC) -c -o $@ $<

$(BUILD)/tui_main.o: tui/main.cpp tui/editor.h tui/nav.h $(SHIM) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(FTXUI_INC) -c -o $@ $<

TESTS := crypto mpq_read mpq_write editor json validate saveutil editor_model nav
TEST_BINS := $(addprefix $(BUILD)/test_,$(TESTS))

all: $(BUILD)/butcher $(TEST_BINS)

$(BUILD):
	@mkdir -p $(BUILD)

# --- unmodified game sources -----------------------------------------------
$(BUILD)/sha.o: $(DEVILUTION)/Source/sha.cpp $(SHIM) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(GAME_FLAGS) -c -o $@ $<

$(BUILD)/codec.o: $(DEVILUTION)/Source/codec.cpp $(SHIM) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(GAME_FLAGS) -c -o $@ $<

$(BUILD)/encrypt.o: $(DEVILUTION)/Source/encrypt.cpp $(SHIM) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(GAME_FLAGS) -c -o $@ $<

# Real spell and item names, rather than a copy that would rot.
$(BUILD)/spelldat.o: $(DEVILUTION)/Source/spelldat.cpp $(SHIM) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(GAME_FLAGS) -c -o $@ $<

$(BUILD)/itemdat.o: $(DEVILUTION)/Source/itemdat.cpp $(SHIM) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(GAME_FLAGS) -c -o $@ $<

# The same two tables again, for Hellfire. -DHELLFIRE selects its data and the
# array name is redefined on the command line so both flavors coexist in one
# binary -- neither file is edited. Safe because SpellData and ItemDataStruct
# have no conditional members, so the type is identical on both sides.
$(BUILD)/spelldat_hf.o: $(DEVILUTION)/Source/spelldat.cpp $(SHIM) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(GAME_FLAGS) -DHELLFIRE -Dspelldata=spelldata_hf -c -o $@ $<

$(BUILD)/itemdat_hf.o: $(DEVILUTION)/Source/itemdat.cpp $(SHIM) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(GAME_FLAGS) -DHELLFIRE -DAllItemsList=AllItemsList_hf -c -o $@ $<

$(BUILD)/explode.o: $(DEVILUTION)/3rdParty/PKWare/explode.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(PKWARE_FLAGS) -c -o $@ $<

$(BUILD)/implode.o: $(DEVILUTION)/3rdParty/PKWare/implode.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(PKWARE_FLAGS) -c -o $@ $<

# Hellfire's compile-time constants, read from the real headers.
$(BUILD)/hellfire.o: src/compat/hellfire.cpp $(SHIM) | $(BUILD)
	$(CXX) $(CXXFLAGS) -DHELLFIRE -c -o $@ $<

# --- our sources ------------------------------------------------------------
$(BUILD)/shim.o: src/compat/shim.cpp $(SHIM) | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/%.o: src/%.cpp $(SHIM) | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/test_%.o: tests/%.cpp $(SHIM) | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# The navigation test drives real FTXUI components, so it links FTXUI.
$(BUILD)/test_nav.o: tests/nav.cpp tui/nav.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $(FTXUI_INC) -c -o $@ $<

$(BUILD)/test_nav: $(BUILD)/test_nav.o $(BUILD)/tui_nav.o $(FTXUI_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

# The editor-model test links the TUI's model, but not FTXUI.
$(BUILD)/test_editor_model: $(BUILD)/test_editor_model.o $(BUILD)/tui_editor.o \
		$(LIB_OBJ) $(GAME_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

# --- links ------------------------------------------------------------------
$(BUILD)/main.o: cli/main.cpp $(SHIM) | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# One binary carrying both front ends. src/butcher.cpp picks between them.
$(BUILD)/butcher.o: src/butcher.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/butcher: $(BUILD)/butcher.o $(BUILD)/main.o $(BUILD)/tui_main.o \
		$(BUILD)/tui_editor.o $(BUILD)/tui_nav.o $(LIB_OBJ) $(GAME_OBJ) $(FTXUI_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

# The validate tests drive the built binary, so they depend on it.
$(BUILD)/test_validate: $(BUILD)/test_validate.o $(LIB_OBJ) $(GAME_OBJ) $(BUILD)/butcher
	$(CXX) $(CXXFLAGS) -o $@ $(filter %.o,$^)

$(BUILD)/test_%: $(BUILD)/test_%.o $(LIB_OBJ) $(GAME_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

# --- tests ------------------------------------------------------------------
# Each test gets its own scratch directory so a stray file from one cannot
# affect another.
check: all
	@set -e; for t in $(TESTS); do \
		rm -rf $(BUILD)/scratch_$$t && mkdir -p $(BUILD)/scratch_$$t; \
		BUTCHER_TMPDIR=$(BUILD)/scratch_$$t \
		BUTCHER_EXE=./$(BUILD)/butcher \
		./$(BUILD)/test_$$t; \
	done

# --- sanitizers and fuzzing --------------------------------------------------
# Every input butcher reads is a file someone else made, and the bounds checks
# on it are written by hand. These run the same code under AddressSanitizer and
# UndefinedBehaviorSanitizer, in a separate build tree so the normal one is
# left alone.
#
# SANITIZERS narrows the set where one is unavailable. Apple's ASan runtime in
# Xcode's clang 17 hangs during its own startup on macOS 26 -- before main, on
# an empty program -- so there `make asan SANITIZERS=undefined` is what runs;
# CI covers address on Linux.
SANITIZERS ?= address,undefined
ASAN_FLAGS := -O1 -g -fno-omit-frame-pointer -fsanitize=$(SANITIZERS) \
	-fno-sanitize-recover=undefined
# One tree per sanitizer set: objects do not depend on the flags that built
# them, so sharing a tree would link stale ones.
comma := ,
SAN_BUILD := build-san-$(subst $(comma),-,$(SANITIZERS))

# Leak checking is off for the suites: they are short-lived test programs, and
# a leak there is not a defect in a save. The fuzzers keep it on, where a leak
# on an error path in the library would be.
asan:
	ASAN_OPTIONS=detect_leaks=0 $(MAKE) BUILD=$(SAN_BUILD) SANITIZE="$(ASAN_FLAGS)" check

# The harnesses in fuzz/ meet libFuzzer's interface. Linked with driver.cpp
# they mutate a built-in seed on any compiler; linked with libFuzzer they are
# coverage-guided. FUZZ_ENGINE selects which.
FUZZERS := save game json
FUZZ_BINS := $(addprefix $(BUILD)/fuzz_,$(FUZZERS))
FUZZ_ENGINE ?=
FUZZ_DRIVER := $(if $(FUZZ_ENGINE),,$(BUILD)/fuzz_driver.o)
FUZZ_ITERATIONS ?= 20000

$(BUILD)/fuzz_%.o: fuzz/%.cpp fuzz/fuzz.h $(SHIM) | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/fuzz_%: $(BUILD)/fuzz_%.o $(FUZZ_DRIVER) $(LIB_OBJ) $(GAME_OBJ)
	$(CXX) $(CXXFLAGS) $(FUZZ_ENGINE) -o $@ $^

fuzz-bins: $(FUZZ_BINS)

# Otherwise make treats the harness objects as intermediates and deletes them,
# rebuilding all three on every run.
.PRECIOUS: $(BUILD)/fuzz_%.o

# The standalone driver, sanitized. Quick enough to run before a commit.
fuzz-smoke:
	$(MAKE) BUILD=$(SAN_BUILD) SANITIZE="$(ASAN_FLAGS)" fuzz-bins
	@set -e; for f in $(FUZZERS); do \
		./$(SAN_BUILD)/fuzz_$$f --iterations $(FUZZ_ITERATIONS); \
	done

# Coverage-guided, with libFuzzer. Needs a clang that ships it -- LLVM's own,
# not Apple's: FUZZ_CXX=/path/to/clang++ make fuzz. Each harness starts from
# the seed the driver writes and runs for FUZZ_SECONDS.
FUZZ_CXX ?= clang++
FUZZ_SECONDS ?= 60

fuzz: fuzz-smoke
	$(MAKE) BUILD=build-fuzz CXX="$(FUZZ_CXX)" FUZZ_ENGINE=-fsanitize=fuzzer \
		SANITIZE="$(ASAN_FLAGS) -fsanitize=fuzzer-no-link" fuzz-bins
	@set -e; for f in $(FUZZERS); do \
		mkdir -p build-fuzz/corpus_$$f; \
		./$(SAN_BUILD)/fuzz_$$f --write-seed build-fuzz/corpus_$$f; \
		./build-fuzz/fuzz_$$f -max_total_time=$(FUZZ_SECONDS) \
			-artifact_prefix=build-fuzz/crash_$${f}_ build-fuzz/corpus_$$f; \
	done

install: $(BUILD)/butcher
	@mkdir -p "$(DESTDIR)$(BINDIR)"
	install -m 755 $(BUILD)/butcher "$(DESTDIR)$(BINDIR)/butcher"
	@echo "installed $(DESTDIR)$(BINDIR)/butcher"

uninstall:
	@rm -f "$(DESTDIR)$(BINDIR)/butcher"
	@echo "removed $(DESTDIR)$(BINDIR)/butcher"

clean:
	@rm -rf $(BUILD) build-san-* build-fuzz

.PHONY: all check clean install uninstall asan fuzz fuzz-bins fuzz-smoke

# Written by -MMD above. Absent on a clean tree, hence the leading dash.
-include $(wildcard $(BUILD)/*.d $(BUILD)/ftxui/*.d)
