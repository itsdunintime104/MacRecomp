# Compile and link the generated C into build.nosync/<game id>.
# Run as: make -C build -j$(sysctl -n hw.ncpu)
# ROOT is the engine; GAME_ROOT (the folder of game.toml) holds the game's inputs and outputs.
ROOT := $(abspath ..)
# The engine is the engine/ folder of a game repo, so game.toml sits above it.
GAME ?= $(ROOT)/../game.toml
GAME_ROOT := $(patsubst %/,%,$(dir $(abspath $(GAME))))
GEN := $(GAME_ROOT)/generated.nosync
OUT := $(GAME_ROOT)/build.nosync
OBJ := $(OUT)/obj
GAME_OUT := $(OUT)/game
PY := $(GAME_ROOT)/.venv/bin/python

# Per-game build inputs (headers, native overrides, game.mk) derived from game.toml.
# game.mk defines GAME_ID and FE_LIFTED_RE; derive writes only the files that changed,
# so touch keeps make from running it again.
.DEFAULT_GOAL := all
include $(GAME_OUT)/game.mk
$(GAME_OUT)/game.mk: $(GAME) $(ROOT)/tools/game/feral_frontend.toml $(wildcard $(ROOT)/tools/game/*.py) $(GAME_DEPS) $(ROOT)/tools/hobbit/emit_runtime.py $(ROOT)/tools/hobbit/macho.py
	cd $(ROOT) && $(PY) -m tools.game.derive --profile $(abspath $(GAME)) --out $(GAME_OUT)
	touch $@

game: $(GAME_OUT)/game.mk

CC := clang
# TSO=1 (default): lifted guest loads/stores keep x86 ordering (ldapr/stlr).
# TSO=0 is plain loads/stores, for bisecting or timing. Make does not track
# CFLAGS: delete build.nosync/obj/*.o after changing it.
TSO ?= 1
# OPT: clang optimisation level for the game code and the runtime (the overlay shows it).
OPT ?= 2
CFLAGS := -std=gnu11 -DRECOMP_TSO=$(TSO) -O$(OPT) -DRECOMP_OPT=$(OPT) -fwrapv -fno-strict-aliasing -ffp-contract=off -arch arm64 -I $(ROOT)/tools/xrecomp/include -I $(GEN) -I $(GAME_OUT) -I $(ROOT)/runtime/include -w -DRECOMP_GLUE_NO_MAIN \
          -I /opt/homebrew/opt/sdl2/include/SDL2 -D_THREAD_SAFE

LDFLAGS_FW := -framework OpenGL -framework CoreFoundation -framework ApplicationServices \
              -framework Carbon -framework IOKit -framework AudioToolbox -framework CoreAudio \
              -L /opt/homebrew/opt/sdl2/lib -lSDL2

GEN_SRCS := $(wildcard $(GEN)/*.c)
# Every runtime source. Native replacements of lifted functions are listed in
# build.nosync/game/native_overrides.txt; the lifter declares those weak.
RT_SRCS := $(wildcard $(ROOT)/runtime/src/*.c)
STUB_SRC := $(OUT)/import_stubs.c

# sample.c repeats 200 functions that the chunk files define (a --sample
# leftover). It is compiled as a check but not linked, or ld sees duplicates.
SAMPLE_OBJ := $(OBJ)/gen_sample.o
OBJS := $(filter-out $(SAMPLE_OBJ),$(patsubst $(GEN)/%.c,$(OBJ)/gen_%.o,$(GEN_SRCS))) \
        $(patsubst $(ROOT)/runtime/src/%.c,$(OBJ)/rt_%.o,$(RT_SRCS)) \
        $(OBJ)/import_stubs.o

HEADERS := $(wildcard $(GEN)/*.h) $(wildcard $(GAME_OUT)/*.h) $(wildcard $(ROOT)/runtime/include/*.h) $(ROOT)/tools/xrecomp/include/recomp_types.h

all: $(OUT)/$(GAME_ID) $(OUT)/cghelper $(if $(wildcard $(GEN)/sample.c),$(SAMPLE_OBJ))

# x86_64 helper that runs the game's bundled Cg runtime under Rosetta
# (runtime/src/thunk_cg.c starts it from next to the executable).
$(OUT)/cghelper: $(ROOT)/tools/cghelper/cghelper.c $(ROOT)/tools/cghelper/cg_wire.h | $(OBJ)
	$(CC) -std=gnu11 -O1 -arch x86_64 -o $@ $<

$(OUT)/$(GAME_ID): $(OBJS)
	$(CC) -arch arm64 -o $@ $^ $(LDFLAGS_FW)

$(OBJ):
	mkdir -p $(OBJ)

$(OBJ)/gen_%.o: $(GEN)/%.c $(HEADERS) | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

# The chunk(s) that define the frontend functions frontend_sdl.c replaces; the header
# renames their lifted definitions so callers bind to frontend_sdl.c.
RENAME_OBJS := $(patsubst $(GEN)/%.c,$(OBJ)/gen_%.o,$(shell grep -lE '^void ($(FE_LIFTED_RE))\(void\)$$' $(GEN)/*.c 2>/dev/null))
$(RENAME_OBJS): CFLAGS += -include $(GAME_OUT)/frontend_rename.h

# HEAP_DEBUG=1 adds dlmalloc's per-call heap checks; a failed check prints its
# expression and line. Delete $(OBJ)/rt_dlmalloc.o when you switch it.
HEAP_DEBUG ?= 0
ifeq ($(HEAP_DEBUG),1)
$(OBJ)/rt_dlmalloc.o: CFLAGS += -DDEBUG=1 -DABORT_ON_ASSERT_FAILURE=0
endif

$(OBJ)/rt_%.o: $(ROOT)/runtime/src/%.c $(HEADERS) | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(STUB_SRC): $(GEN)/imports.h $(IMPORT_SIGS) $(ROOT)/tools/hobbit/emit_import_stubs.py | $(OBJ)
	python3 -I $(ROOT)/tools/hobbit/emit_import_stubs.py $(GEN)/imports.h $(IMPORT_SIGS) $@

$(OBJ)/import_stubs.o: $(STUB_SRC) $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Double-clickable launcher bundle in "build.nosync/<app name>.app".
app: $(OUT)/$(GAME_ID) $(OUT)/cghelper
	PYTHON=$(PY) $(ROOT)/tools/package/make_app.sh $(abspath $(GAME))

.PHONY: all app game
