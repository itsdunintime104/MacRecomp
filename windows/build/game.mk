# Build rules for a Windows (PE32) game repo: compile the lifted C (GEN), the
# engine's generic runtime (runtime/src), the Windows runtime (windows/runtime/src:
# every Win32 slice and the D3D9/D3DX layer) and the game's own sources into
# build.nosync/$(GAME_ID), the native game. Interface: windows/README.md.
#
# The game repo's build/Makefile sets the variables below, then includes this
# file; run it as make -C build -j8 [OPT=1] [GEN=<lift output dir>] [all|app|test].
#   ENGINE ?= $(abspath ../engine)
#   GAME_SRCS := <game .c files>             optional (absolute paths)
#   GAME_CFLAGS := -DPE_ENTRY_RVA=<rva>u -DPE_SECTION_COUNT=<n>u [more -D/-I]   required
#   GAME_TESTS := <tests file for run_all.sh --tests>                         optional
#   include $(ENGINE)/windows/build/game.mk
# GAME_ID (the binary name) and the APP_* names come from the game repo's
# game.conf (GAME_CONF), the file setup.sh reads, unless build/Makefile or the
# command line sets them. The graphics libraries come from windows/tools/deps (DEPS).
GAME_ROOT ?= $(abspath ..)
ENGINE ?= $(GAME_ROOT)/engine
WIN := $(ENGINE)/windows
GEN ?= $(GAME_ROOT)/generated.nosync
OUT := $(GAME_ROOT)/build.nosync
OBJ := $(OUT)/obj

GAME_CONF ?= $(GAME_ROOT)/game.conf
APP_VARS := APP_NAME APP_EXE APP_ID APP_SAVE_NAME APP_ENV_PREFIX APP_GAME_FILE APP_GAME_TITLE APP_IMAGE GAME_REG_KEY
ifneq ($(wildcard $(GAME_CONF)),)
conf_value = $(shell . "$(GAME_CONF)" && printf '%s' "$$$(1)")
$(foreach v,GAME_ID $(APP_VARS),$(if $(filter undefined,$(origin $(v))),$(eval $(v) := $(call conf_value,$(v)))))
endif
$(foreach v,GAME_ID $(APP_VARS),$(if $(strip $($(v))),,$(error set $(v) in game.conf or build/Makefile)))

DEPS ?= $(OUT)/deps
DXVK_PREFIX ?= $(DEPS)/dxvk
VKD3D_PREFIX ?= $(DEPS)/vkd3d
KK_PREFIX ?= $(DEPS)/kosmickrisp
BREW_PREFIX ?= /opt/homebrew
FAUDIO := $(BREW_PREFIX)/opt/faudio

CC := clang
# TSO=1 (default): lifted guest loads/stores keep x86 ordering (ldapr/stlr).
TSO ?= 1
OPT ?= 2
CFLAGS := -std=gnu11 -DRECOMP_TSO=$(TSO) -O$(OPT) -DRECOMP_OPT=$(OPT) -fwrapv -fno-strict-aliasing -ffp-contract=off -arch arm64 -I $(ENGINE)/tools/xrecomp/include -I $(GEN) -I $(WIN)/runtime/include -I $(ENGINE)/runtime/include -w -DRECOMP_GLUE_NO_MAIN
# Host headers for the Windows runtime and the game sources: DXVK, SDL2,
# FAudio, vkd3d-shader; then the game's settings (GAME_SAVE_NAME from
# APP_SAVE_NAME, GAME_EXE_NAME from APP_GAME_FILE, GAME_REG_KEY with each \ doubled
# for C, and GAME_CFLAGS).
HOST_CFLAGS := -I $(DXVK_PREFIX)/include/dxvk -I $(BREW_PREFIX)/include -I $(BREW_PREFIX)/include/SDL2 -I $(FAUDIO)/include -I $(VKD3D_PREFIX)/include -I $(VKD3D_PREFIX)/include/vkd3d \
	-DGAME_SAVE_NAME='"$(APP_SAVE_NAME)"' -DGAME_EXE_NAME='"$(APP_GAME_FILE)"' \
	-DGAME_REG_KEY='"$(subst \,\\,$(GAME_REG_KEY))"' $(GAME_CFLAGS)
LDLIBS := -L$(DXVK_PREFIX)/lib -ldxvk_d3d9 -Wl,-rpath,$(DXVK_PREFIX)/lib \
	-L$(VKD3D_PREFIX)/lib -lvkd3d-shader -Wl,-rpath,$(VKD3D_PREFIX)/lib \
	-L$(FAUDIO)/lib -lFAudio -Wl,-rpath,$(FAUDIO)/lib \
	-L$(BREW_PREFIX)/lib -lSDL2 -framework CoreGraphics -framework CoreFoundation -lobjc -lc++

# Generic engine runtime: guest memory, glue, guest calls, host functions,
# heap, diagnostics, overlay text.
ENGINE_RT := recomp_glue guest_mem guest_call host_funcs guest_heap dlmalloc diag hud_core
# Windows runtime: every file in windows/runtime/src.
WIN_SRCS := $(wildcard $(WIN)/runtime/src/*.c)

GEN_SRCS := $(wildcard $(GEN)/*.c)
GEN_OBJS := $(patsubst $(GEN)/%.c,$(OBJ)/gen_%.o,$(GEN_SRCS))
ENGINE_OBJS := $(patsubst %,$(OBJ)/rt_%.o,$(ENGINE_RT))
WIN_OBJS := $(patsubst $(WIN)/runtime/src/%.c,$(OBJ)/win_%.o,$(WIN_SRCS))
# Game sources may sit in any folders; objects are named by file name.
GAME_OBJS := $(patsubst %.c,$(OBJ)/game_%.o,$(notdir $(GAME_SRCS)))
vpath %.c $(sort $(dir $(GAME_SRCS)))

GEN_HEADERS := $(wildcard $(GEN)/*.h) $(ENGINE)/tools/xrecomp/include/recomp_types.h
RT_HEADERS := $(wildcard $(WIN)/runtime/include/*.h) $(wildcard $(ENGINE)/runtime/include/*.h) $(ENGINE)/tools/xrecomp/include/recomp_types.h

# Make does not track CFLAGS, ENGINE or GEN. These stamps change when they do, so
# switching GEN rebuilds the generated objects and switching OPT, TSO or ENGINE
# rebuilds everything.
FLAGS_STAMP := $(OBJ)/flags.stamp
GEN_STAMP := $(OBJ)/gen.stamp
$(shell mkdir -p $(OBJ); \
  [ "$$(cat $(FLAGS_STAMP) 2>/dev/null)" = "O$(OPT) TSO$(TSO) $(ENGINE)" ] || echo "O$(OPT) TSO$(TSO) $(ENGINE)" > $(FLAGS_STAMP); \
  [ "$$(cat $(GEN_STAMP) 2>/dev/null)" = "$(GEN)" ] || echo "$(GEN)" > $(GEN_STAMP))

all: $(OUT)/$(GAME_ID)

# -export_dynamic: crash backtraces name the lifted functions (sub_<VA>).
# The header padding lets windows/tools/package/make_app.sh rewrite install names and rpaths.
$(OUT)/$(GAME_ID): $(GEN_OBJS) $(ENGINE_OBJS) $(WIN_OBJS) $(GAME_OBJS)
	$(CC) -arch arm64 -Wl,-export_dynamic -Wl,-headerpad_max_install_names -o $@ $^ $(LDLIBS)

$(OBJ)/gen_%.o: $(GEN)/%.c $(GEN_HEADERS) $(FLAGS_STAMP) $(GEN_STAMP)
	$(CC) $(CFLAGS) -c $< -o $@

$(ENGINE_OBJS): $(OBJ)/rt_%.o: $(ENGINE)/runtime/src/%.c $(RT_HEADERS) $(FLAGS_STAMP)
	$(CC) $(CFLAGS) -c $< -o $@

$(WIN_OBJS): $(OBJ)/win_%.o: $(WIN)/runtime/src/%.c $(RT_HEADERS) $(FLAGS_STAMP)
	$(CC) $(CFLAGS) $(HOST_CFLAGS) -c $< -o $@

$(OBJ)/game_%.o: %.c $(RT_HEADERS) $(FLAGS_STAMP)
	$(CC) $(CFLAGS) $(HOST_CFLAGS) -c $< -o $@

# Build and run every Windows runtime test (windows/tests/run_all.sh), plus the
# game's own tests when GAME_TESTS names its test list. More options go in
# TEST_ARGS, for example TEST_ARGS=--no-gpu.
TEST_ARGS ?=
test:
	RECOMP_DEPS_DIR="$(DEPS)" $(WIN)/tests/run_all.sh $(if $(GAME_TESTS),--tests "$(GAME_TESTS)") $(TEST_ARGS)

# Double-clickable bundle "build.nosync/$(APP_NAME).app" with every non-system
# library inside it (windows/tools/package/make_app.sh; check it with
# windows/tools/package/check_app.sh). The launcher uses GAME_DIR (the game
# folder) and GAME_IMAGE (the game image) by default. make_app.sh reads the
# APP_* settings from the environment.
GAME_DIR ?=
GAME_IMAGE ?= $(GAME_ROOT)/orig.nosync/$(APP_IMAGE)
export APP_EXE APP_ID APP_SAVE_NAME APP_ENV_PREFIX APP_GAME_FILE APP_GAME_TITLE APP_IMAGE

app: $(OUT)/$(GAME_ID)
	DXVK_PREFIX="$(DXVK_PREFIX)" VKD3D_PREFIX="$(VKD3D_PREFIX)" KK_PREFIX="$(KK_PREFIX)" \
	BREW_PREFIX="$(BREW_PREFIX)" GAME_DIR="$(GAME_DIR)" GAME_IMAGE="$(GAME_IMAGE)" \
	$(WIN)/tools/package/make_app.sh "$(OUT)/$(GAME_ID)" "$(OUT)/$(APP_NAME).app"

.PHONY: all test app
