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
CFLAGS  += -Iinclude -Isrc
LDFLAGS +=

# Objects live under build/$(PORT)/ so host and cross builds cannot collide:
# a leftover x86 main.o in a device link fails late and confusingly.
BUILD := build/$(PORT)
SRC   := src/main.c src/core.c src/env.c src/scale.c src/audio.c src/save.c src/proto.c src/options.c port/$(PORT).c
OBJ   := $(SRC:%.c=$(BUILD)/%.o)
BIN   := $(BUILD)/diatom

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

.PHONY: all clean check-seam stub run-stub tools probes

all: $(BIN)

# Measurement instruments (tools/). Not part of the frontend, never linked into
# it, and deliberately not built by `all`. Cores and ROMs are supplied locally;
# see tools/README.md.
TOOLS_DIR := $(BUILD)/tools
TOOLS     := $(TOOLS_DIR)/envlog $(TOOLS_DIR)/rssprobe
TOOL_CFLAGS := -std=gnu11 -Wall -Wextra -Wno-unused-parameter -O1 -Isrc -I$(TOOLS_DIR)

tools: $(TOOLS)

$(TOOLS_DIR)/env_names.h: src/libretro.h tools/gen_env_names.py
	@mkdir -p $(TOOLS_DIR)
	python3 tools/gen_env_names.py src/libretro.h > $@

$(TOOLS_DIR)/envlog: tools/envlog.c $(TOOLS_DIR)/env_names.h
	@mkdir -p $(TOOLS_DIR)
	$(CC) $(TOOL_CFLAGS) -o $@ $< $(TOOL_LDFLAGS)

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

$(BIN): $(OBJ)
	@mkdir -p $(BUILD)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# The seam test from ADR-0007, mechanised. A port that includes libretro.h can
# no longer be built without a core, which destroys the reason the desktop
# backend exists. Cheap to check, so check it.
check-seam:
	@if grep -nE '^[[:space:]]*#[[:space:]]*include.*libretro\.h' \
	        port/*.c include/*.h 2>/dev/null; then \
		echo "FAIL: a port or the port header includes libretro.h (ADR-0007)"; \
		exit 1; \
	else \
		echo "ok: no port includes libretro.h"; \
	fi

clean:
	rm -rf build
