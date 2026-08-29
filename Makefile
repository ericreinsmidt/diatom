# Diatom.
#
# Port selection is COMPILE-TIME: one binary per device, the port linked in.
# No plugin mechanism -- nobody swaps a device backend at runtime on a handheld,
# and an abstraction with no consumer is a tax (register §0).
#
#   make                 desktop build (SDL2), the development target
#   make PORT=brick      device build for the TrimUI Brick (TG3040); needs the
#                        cross toolchain, so run it as  tools/brick-make.sh

PORT ?= desktop

CC      ?= cc
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -O2
CFLAGS  += -Iinclude -Isrc -Ivendor/rcheevos/include
# Header dependency tracking. Without it, changing a struct in diatom.h leaves
# stale objects calling through old member offsets - which on 2026-08-26 turned
# serialize_size() into a call to a different function entirely and made state
# saving report "no state" while every freshly built file was correct.
CFLAGS  += -MMD -MP
LDFLAGS +=

# Objects live under build/$(PORT)/ so host and cross builds cannot collide:
# a leftover x86 main.o in a device link fails late and confusingly.
BUILD := build/$(PORT)
SRC   := src/main.c src/cheevos.c src/core.c src/env.c src/scale.c src/audio.c src/save.c src/proto.c src/options.c src/zip.c port/$(PORT).c
OBJ   := $(SRC:%.c=$(BUILD)/%.o)
BIN   := $(BUILD)/diatom
CONFORM := $(BUILD)/diatom-conform

# rcheevos, vendored under ADR-0025. Listed rather than wildcarded: a file
# appearing in vendor/ should be a decision recorded in its README, not
# something the build picks up because it happens to be on disk.
RC_DIR := vendor/rcheevos
RC_SRC := \
  $(RC_DIR)/src/rc_compat.c $(RC_DIR)/src/rc_util.c $(RC_DIR)/src/rc_version.c \
  $(RC_DIR)/src/rhash/md5.c \
  $(RC_DIR)/src/rcheevos/alloc.c $(RC_DIR)/src/rcheevos/condition.c \
  $(RC_DIR)/src/rcheevos/condset.c $(RC_DIR)/src/rcheevos/consoleinfo.c \
  $(RC_DIR)/src/rcheevos/format.c $(RC_DIR)/src/rcheevos/lboard.c \
  $(RC_DIR)/src/rcheevos/memref.c $(RC_DIR)/src/rcheevos/operand.c \
  $(RC_DIR)/src/rcheevos/rc_validate.c $(RC_DIR)/src/rcheevos/richpresence.c \
  $(RC_DIR)/src/rcheevos/runtime.c $(RC_DIR)/src/rcheevos/runtime_progress.c \
  $(RC_DIR)/src/rcheevos/trigger.c $(RC_DIR)/src/rcheevos/value.c
RC_OBJ := $(RC_SRC:%.c=$(BUILD)/%.o)

# Someone else's code, compiled on its own terms. -Wall -Wextra is a rule this
# project holds itself to; applying it to a dependency only produces noise that
# cannot be fixed here, because vendor/rcheevos/README.md forbids patching -
# a local fix there goes invisible at the next update.
#
# -DRC_DISABLE_LUA is not a preference: rich presence is the only thing that
# wants Lua, Diatom has no Lua, and upstream's own package sets the same define.
RC_CFLAGS := -std=gnu11 -O2 -MMD -MP -DRC_DISABLE_LUA \
             -I$(RC_DIR)/include -I$(RC_DIR)/src
$(RC_OBJ): CFLAGS := $(RC_CFLAGS)

ifeq ($(PORT),desktop)
  # sdl2-config ships with SDL2 itself; pkg-config is a separate install and is
  # not always present. Prefer the one that comes with the dependency.
  SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null || pkg-config --cflags sdl2 2>/dev/null)
  SDL_LIBS   := $(shell sdl2-config --libs   2>/dev/null || pkg-config --libs   sdl2 2>/dev/null)
  ifeq ($(strip $(SDL_LIBS)),)
    $(error SDL2 not found: install it, or put sdl2-config/pkg-config on PATH)
  endif
  CFLAGS  += $(SDL_CFLAGS)
  LDFLAGS += $(SDL_LIBS)
  # dlopen lives in libc on macOS and on modern glibc; -ldl is harmless where
  # it exists and absent where it does not.
  ifeq ($(shell uname -s),Linux)
    LDFLAGS += -ldl -lpthread
  endif
endif

ifeq ($(PORT),brick)
  # TrimUI Brick (TG3040) - ADR-0012. Stock Debian cross-compiler; SDL2 is the
  # device's own library plus version-matched upstream headers, assembled into
  # sysroot/brick by tools/fetch-brick-sysroot.sh and never committed.
  CROSS   ?= aarch64-linux-gnu-
  CC       = $(CROSS)gcc
  SYSROOT ?= sysroot/brick
  ifeq ($(wildcard $(SYSROOT)/usr/trimui/lib/libSDL2.so),)
    $(error brick sysroot missing: run tools/fetch-brick-sysroot.sh)
  endif
  CFLAGS  += -I$(SYSROOT)/usr/include/SDL2 -D_REENTRANT
  LDFLAGS += -L$(SYSROOT)/usr/trimui/lib -Wl,-rpath-link,$(SYSROOT)/usr/trimui/lib
  # Explicit -ldl/-lpthread: the toolchain's glibc 2.31 predates their merge
  # into libc proper (2.34).
  LDFLAGS += -lSDL2 -lm -ldl -lpthread
endif

.PHONY: all clean check check-seam check-register check-register-diff \
        check-corefacts check-cheevos stub run-stub tools probes

all: $(BIN)

# Measurement instruments (tools/). Not part of the frontend, never linked into
# it, and deliberately not built by `all`. Cores and ROMs are supplied locally;
# see tools/README.md.
TOOLS_DIR := $(BUILD)/tools
TOOLS     := $(TOOLS_DIR)/envlog $(TOOLS_DIR)/rssprobe $(TOOLS_DIR)/resampleprobe
TOOL_CFLAGS := -std=gnu11 -Wall -Wextra -Wno-unused-parameter -O1 -Isrc -I$(TOOLS_DIR)

tools: $(TOOLS)

$(TOOLS_DIR)/env_names.h: src/libretro.h tools/gen_env_names.py
	@mkdir -p $(TOOLS_DIR)
	python3 tools/gen_env_names.py src/libretro.h > $@

$(TOOLS_DIR)/envlog: tools/envlog.c $(TOOLS_DIR)/env_names.h
	@mkdir -p $(TOOLS_DIR)
	$(CC) $(TOOL_CFLAGS) -o $@ $< $(TOOL_LDFLAGS)

$(TOOLS_DIR)/resampleprobe: tools/resampleprobe.c src/audio.c
	@mkdir -p $(TOOLS_DIR)
	$(CC) $(TOOL_CFLAGS) -Iinclude -o $@ $^ -lm $(TOOL_LDFLAGS)

$(TOOLS_DIR)/rssprobe: tools/rssprobe.c
	@mkdir -p $(TOOLS_DIR)
	$(CC) $(TOOL_CFLAGS) -o $@ $< $(TOOL_LDFLAGS)

ifeq ($(shell uname -s),Linux)
  TOOL_LDFLAGS += -ldl
endif

# Device probes. Every number they produce is cited by an ADR, which is exactly
# why they are versioned rather than left in a temp directory: an unverifiable
# measurement is the drift this project exists to avoid (practice 7).
#
# They only build for PORT=brick - they use linux/fb.h, the device's SDL2, or
# both, and their results only mean anything on the hardware they measure.
#
#   tools/brick-make.sh probes
PROBE_SRC := savprobe wprobe sigprobe pantest holdfb warmprobe protodrive blitprobe
PROBES    := $(addprefix $(TOOLS_DIR)/,$(PROBE_SRC)) $(TOOLS_DIR)/eglpresent

probes: $(PROBES)

$(TOOLS_DIR)/savprobe: tools/savprobe.c
	@mkdir -p $(TOOLS_DIR)
	$(CC) $(TOOL_CFLAGS) -o $@ $< $(TOOL_LDFLAGS)

$(TOOLS_DIR)/%: tools/%.c
	@mkdir -p $(TOOLS_DIR)
	$(CC) $(TOOL_CFLAGS) -o $@ $<

$(TOOLS_DIR)/eglpresent: tools/eglpresent.c
	@mkdir -p $(TOOLS_DIR)
	$(CC) $(TOOL_CFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS)

# A libretro core that is not an emulator, so Diatom can be exercised end to end
# with no third-party binary present. See test/stubcore.c.
STUB := $(BUILD)/stubcore.so

stub: $(STUB)

$(STUB): test/stubcore.c
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -Wall -Wextra -Wno-unused-parameter -O2 -Isrc \
	      -shared -fPIC -o $@ $< -lm

run-stub: $(BIN) $(STUB)
	./$(BIN) --core $(STUB)

$(BIN): $(OBJ) $(RC_OBJ)
	@mkdir -p $(BUILD)
	$(CC) -o $@ $(OBJ) $(RC_OBJ) $(LDFLAGS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# The seam test from ADR-0007, mechanised. A port that includes libretro.h can
# no longer be built without a core, which destroys the reason the desktop
# backend exists. Cheap to check, so check it.
# Two mechanical checks, both guarding a rule that decays the moment nothing
# fails when it is broken. check-seam has held since day one for exactly that
# reason; the register drifted 418 -> 992 lines in three days because nothing
# ever complained.
check: check-seam check-register check-corefacts check-rates check-cheevos

# Does a RetroAchievements address reach the byte it names? Offline, needs no
# core and no ROM, and links only cheevos.c plus the vendored runtime - so it
# belongs in `check` rather than beside the conformance suite. It is also the
# only thing that fails if the vendored library is updated and the mapping
# stops agreeing with it.
CHEEVOS_TEST := $(BUILD)/cheevos-test

check-cheevos: $(CHEEVOS_TEST)
	@./$(CHEEVOS_TEST)

$(CHEEVOS_TEST): test/cheevos_test.c src/cheevos.c $(RC_OBJ)
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -Wall -Wextra -Wno-unused-parameter -O2 \
	      -Iinclude -Isrc -I$(RC_DIR)/include -DRC_DISABLE_LUA \
	      -o $@ test/cheevos_test.c src/cheevos.c $(RC_OBJ)

# Deliberately NOT part of `check`. It needs a build and it runs in real time -
# Diatom paces to the core's frame rate, so 300 frames costs five seconds of
# wall clock and the suite costs about twenty. `check` is what runs before every
# commit and it stays instant and offline; this is what runs before a release or
# after touching the frame loop, and it is named in README so it does not
# quietly stop being run.
conform-check: $(BIN)
	@python3 test/conform.py
.PHONY: conform-check

check-register:
	@python3 tools/check-register.py

# The offline half: does core-facts.md still describe the cores we ship? The
# real proof re-measures on hardware - tools/corefacts.sh --check - but that
# needs the device and the user's ROMs, so it cannot run on every build.
check-corefacts:
	@python3 tools/check-corefacts.py

# core-facts.md is generated so rates are not recalled from memory. This is what
# makes that true of the rest of the tree: a spike table once had Game Boy at
# 32768 Hz, a rate no pinned core produces, and the hardest ratio in the matrix
# went untested because of it.
check-rates:
	@python3 tools/check-rates.py

# Use in a commit hook or by hand before committing: enforces that a resolution
# REPLACES the question it answers rather than being appended below it, which
# is the habit that broke the register.
check-register-diff:
	@python3 tools/check-register.py --diff

check-seam:
	@if grep -nE '^[[:space:]]*#[[:space:]]*include.*libretro\.h' \
	        port/*.c include/*.h 2>/dev/null; then \
		echo "FAIL: a port or the port header includes libretro.h (ADR-0007)"; \
		exit 1; \
	else \
		echo "ok: no port includes libretro.h"; \
	fi
	@# ADR-0025 made rcheevos a second thing on the frontend's side of the
	@# seam, and named this as the check to look at. A port reaching for it
	@# would be reaching past the frontend for the core's memory.
	@if grep -nE '^[[:space:]]*#[[:space:]]*include.*(rc_|rcheevos|rhash)' \
	        port/*.c include/*.h 2>/dev/null; then \
		echo "FAIL: a port or the port header includes rcheevos (ADR-0025)"; \
		exit 1; \
	else \
		echo "ok: no port includes rcheevos"; \
	fi

clean:
	rm -rf build

# The conformance build: identical to $(BIN) but with its own allocations
# counted. Separate target rather than a flag, so nothing test-shaped is linked
# into what ships. --wrap is a GNU ld feature; Apple's ld64 has no equivalent,
# which makes this a Linux and device check. That is the platform the memory
# thesis is actually about.
$(TOOLS_DIR)/allocwatch.o: tools/allocwatch.c
	@mkdir -p $(TOOLS_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(CONFORM): $(OBJ) $(RC_OBJ) $(TOOLS_DIR)/allocwatch.o
	$(CC) -o $@ $^ $(LDFLAGS) \
	  -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free

conform: $(CONFORM)
.PHONY: conform

# The device half of the same suite: allocation counts and the RSS budget, on
# the hardware §11's thesis is actually about. Needs a Brick over adb.
conform-device:
	@tools/conform-device.sh
.PHONY: conform-device

# At the END, after `all:` is established: an included .d file's first rule
# would otherwise become the default goal, and `make` would silently build one
# object and stop - which it did, on 2026-08-26, and looked exactly like the
# docker mtime staleness it was added to prevent.
-include $(OBJ:.o=.d) $(RC_OBJ:.o=.d)
