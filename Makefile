# Diatom.
#
# Port selection is COMPILE-TIME: one binary per device, the port linked in.
# No plugin mechanism -- nobody swaps a device backend at runtime on a handheld,
# and an abstraction with no consumer is a tax (register §0).
#
#   make                 desktop build (SDL2), the development target
#   make PORT=tg5040     device build   (not yet written)

PORT ?= desktop

CC      ?= cc
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -O2
CFLAGS  += -Iinclude -Isrc
LDFLAGS +=

SRC := src/main.c src/core.c src/env.c src/scale.c src/audio.c port/$(PORT).c
OBJ := $(SRC:.c=.o)
BIN := build/diatom

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
    LDFLAGS += -ldl
  endif
endif

.PHONY: all clean check-seam stub run-stub

all: $(BIN)

# A libretro core that is not an emulator, so Diatom can be exercised end to end
# with no third-party binary present. See test/stubcore.c.
STUB := build/stubcore.so

stub: $(STUB)

$(STUB): test/stubcore.c
	@mkdir -p build
	$(CC) -std=gnu11 -Wall -Wextra -Wno-unused-parameter -O2 -Isrc \
	      -shared -fPIC -o $@ $< -lm

run-stub: $(BIN) $(STUB)
	./$(BIN) --core $(STUB)

$(BIN): $(OBJ)
	@mkdir -p build
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c
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
	rm -rf build $(OBJ)
