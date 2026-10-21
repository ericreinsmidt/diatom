# Diatom.
#
# Port selection is COMPILE-TIME: one binary per device, the port linked in.
# No plugin mechanism -- nobody swaps a device backend at runtime on a handheld,
# and an abstraction with no consumer is a tax (register §0).
#
#   make                 desktop build (SDL2), the development target
#   make PORT=brick      device build for the TrimUI Brick
#   make PORT=pixel2     device build for the GKD Pixel 2
#                        Both built by plastron's Buildroot, which supplies CC
#                        and the libraries

PORT ?= desktop

CC      ?= cc
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -O2
CFLAGS  += -Iinclude -Isrc -Ivendor/rcheevos/include
# Header dependency tracking. Without it, changing a struct in diatom.h leaves
# stale objects calling through old member offsets - which on 2026-08-26 turned
# serialize_size() into a call to a different function entirely and made state
# saving report "no state" while every freshly built file was correct.
CFLAGS  += -MMD -MP
# zlib for zipped ROMs (src/zip.c), on every port
LDFLAGS += -lz

# Objects live under build/$(PORT)/ so host and cross builds cannot collide:
# a leftover x86 main.o in a device link fails late and confusingly.
BUILD := build/$(PORT)
SRC   := src/main.c src/cheevos.c src/core.c src/env.c src/scale.c src/audio.c src/save.c src/proto.c src/options.c src/zip.c src/levels.c port/$(PORT).c
OBJ   := $(SRC:%.c=$(BUILD)/%.o)
BIN   := $(BUILD)/diatom
CONFORM := $(BUILD)/diatom-conform

# rcheevos, vendored as the achievement evaluator. Listed rather than
# wildcarded: a file appearing in vendor/ should be a decision recorded in its
# README, not something the build picks up because it happens to be on disk.
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
  # it exists and absent where it does not. libm is separate on Linux, and
  # macOS links it anyway: the resampler's cos() failed to link in a Linux
  # build on 2026-10-01 without it.
  ifeq ($(shell uname -s),Linux)
    LDFLAGS += -ldl -lpthread -lm
  endif
endif

ifeq ($(PORT),brick)
  # TrimUI Brick, on plastron. Built inside plastron's Buildroot, as the Pixel
  # 2's is: it sets CC and puts a pkg-config on PATH that answers for the
  # device system's own SDL2. The display is fbdev and the sound the codec's
  # controls, which need no library.
  PKG_CONFIG ?= pkg-config
  CFLAGS  += $(shell $(PKG_CONFIG) --cflags sdl2)
  LDFLAGS += $(shell $(PKG_CONFIG) --libs sdl2) -lm -ldl -lpthread
endif

ifeq ($(PORT),pixel2)
  # GKD Pixel 2. Built inside plastron's Buildroot, which sets CC and puts a
  # pkg-config on PATH that answers for the device system's own
  # libraries: its SDL2 (audio and the clock), and Mesa's EGL, GLES and GBM
  # with libdrm for the display.
  PKG_CONFIG ?= pkg-config
  PIXEL2_PKGS := sdl2 egl glesv2 gbm libdrm
  CFLAGS  += $(shell $(PKG_CONFIG) --cflags $(PIXEL2_PKGS))
  LDFLAGS += $(shell $(PKG_CONFIG) --libs $(PIXEL2_PKGS)) -lm -ldl -lpthread
endif

.PHONY: all clean check check-seam check-proto check-port \
        check-corefacts check-cheevos stub run-stub

all: $(BIN)


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

# The port interface's seam test, mechanized. A port that includes libretro.h
# can no longer be built without a core, which destroys the reason the desktop
# backend exists. Cheap to check, so check it.
# Mechanical checks, each guarding a rule that decays the moment nothing fails
# when it is broken: check-seam has held since day one for exactly that reason.
check: check-seam check-corefacts check-rates check-cheevos check-proto check-port check-stateplane

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
.PHONY: conform-check check-stateplane

# The state plane is a promise to a launcher in ANOTHER repository, and no
# compiler here can see it: a verb can be in the enum, parsed and dispatched and
# still never reach a socket. So this one speaks the protocol.
#
# It is in `check` despite starting a process, because it costs about a second
# and needs no core, no ROM and no network - and it SKIPS rather than builds
# when build/desktop/diatom is absent, so `check` keeps its promise of being
# instant and offline for someone who has not built yet.
#
# But a build that IS there is brought up to date first. These run the binary
# as it sits in build/desktop, and nothing else rebuilt it: on 2026-10-07
# check-stateplane failed on a DISPLAY field added five days earlier, against a
# binary from the day before that.
FRESH_IF_BUILT = @if [ -e $(BIN) ]; then $(MAKE) --no-print-directory $(BIN) $(STUB); fi

check-proto:
	$(FRESH_IF_BUILT)
	@python3 test/proto.py

# The state plane, end to end against the stub core. On the same terms as
# check-proto: a second of wall time, no core, no ROM, no network.
#
# IT WAS NOT IN `check` UNTIL 2026-09-17, and the cost of that showed the day it
# was added: it had been asserting `proto=3` since the audio state bumped the
# number to 4, and nothing noticed because nothing ran it. A test outside the
# suite is a test that rots while looking like coverage.
check-stateplane:
	$(FRESH_IF_BUILT)
	@python3 test/stateplane.py

# The port's audio contract: falls back, is idempotent, and ALWAYS returns.
# The last one is the reason this exists - a close that joins a stuck audio
# thread is a hang, not a wrong answer, so the test arms an alarm and lets the
# timeout be the failure. Driven against port/desktop.c with SDL's dummy
# drivers: no device, no display, no sound. Also the clock arithmetic both
# ports share, which is the one piece of the Brick's port this can reach.
PORT_TEST := $(BUILD)/port-test

check-port: $(PORT_TEST)
	@./$(PORT_TEST)

$(PORT_TEST): test/port_test.c port/desktop.c port/port_clock.h include/diatom_port.h
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -Wall -Wextra -Wno-unused-parameter -O1 -g \
	      -Iinclude -Isrc -Iport -o $@ test/port_test.c port/desktop.c \
	      $(SDL_CFLAGS) $(SDL_LIBS)

# Does core-facts.md still describe the cores we ship? It was measured on the
# device, which needs the hardware and the user's ROMs; this is the half that
# runs on every build.
check-corefacts:
	@python3 tools/check-corefacts.py

# core-facts.md is generated so rates are not recalled from memory. This is what
# makes that true of the rest of the tree: a spike table once had Game Boy at
# 32768 Hz, a rate no pinned core produces, and the hardest ratio in the matrix
# went untested because of it.
check-rates:
	@python3 tools/check-rates.py

check-seam:
	@if grep -nE '^[[:space:]]*#[[:space:]]*include.*libretro\.h' \
	        port/*.c include/*.h 2>/dev/null; then \
		echo "FAIL: a port or the port header includes libretro.h"; \
		exit 1; \
	else \
		echo "ok: no port includes libretro.h"; \
	fi
	@# Keeping achievements in the frontend made rcheevos a second thing on
	@# the frontend's side of the seam, and this is the check that guards
	@# it. A port reaching for it would be reaching past the frontend for
	@# the core's memory.
	@if grep -nE '^[[:space:]]*#[[:space:]]*include.*(rc_|rcheevos|rhash)' \
	        port/*.c include/*.h 2>/dev/null; then \
		echo "FAIL: a port or the port header includes rcheevos"; \
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
TOOLS_DIR := $(BUILD)/tools
$(TOOLS_DIR)/allocwatch.o: tools/allocwatch.c
	@mkdir -p $(TOOLS_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(CONFORM): $(OBJ) $(RC_OBJ) $(TOOLS_DIR)/allocwatch.o
	$(CC) -o $@ $^ $(LDFLAGS) \
	  -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free

conform: $(CONFORM)
.PHONY: conform

# At the END, after `all:` is established: an included .d file's first rule
# would otherwise become the default goal, and `make` would silently build one
# object and stop - which it did, on 2026-08-26, and looked exactly like the
# docker mtime staleness it was added to prevent.
-include $(OBJ:.o=.d) $(RC_OBJ:.o=.d)
