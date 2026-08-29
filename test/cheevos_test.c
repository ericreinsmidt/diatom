/* SPDX-License-Identifier: MIT */
/* Does a RetroAchievements address reach the byte it is supposed to?
 *
 * ADR-0025 moved achievements into Diatom on the strength of one claim: that
 * conditions compare against the PREVIOUS FRAME, so only something running at
 * frame rate can evaluate them. This is that claim, executed. It runs a real
 * condition out of Blaster Master's set through the vendored runtime, against
 * memory laid out by src/cheevos.c, and checks that it fires on the frame it
 * should and not on the frame before.
 *
 * It is offline and takes no core and no ROM: the "console" here is two byte
 * arrays. That is the point - the mapping is a pure function of what a core
 * declares, so it can be tested without one, and a test that needs hardware is
 * a test that stops being run.
 *
 * Build and run:  make check-cheevos
 */
#include <stdio.h>
#include <string.h>

#include "cheevos.h"
#include "diatom.h"

#include "rc_consoles.h"
#include "rc_runtime.h"

/* cheevos.c logs through the port. Tests have no port, so this is it. */
void diatom_port_log(diatom_log_level lvl, const char *msg)
{
	static const char *n[] = { "DEBUG", "INFO", "WARN", "ERROR" };
	printf("    [%s] %s\n", n[lvl], msg);
}

static int failures;

#define CHECK(cond, ...)                                                      \
	do {                                                                      \
		if (!(cond)) {                                                        \
			printf("  FAIL: ");                                               \
			printf(__VA_ARGS__);                                              \
			printf("\n        at %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
			failures++;                                                       \
		}                                                                     \
	} while (0)

/* ---- a console made of two arrays ---------------------------------------- */

static uint8_t g_ram[0x800];     /* NES work RAM */
static uint8_t g_sram[0x2000];   /* cartridge RAM */

static void *mem_data(unsigned id)
{
	if (id == RETRO_MEMORY_SYSTEM_RAM) return g_ram;
	if (id == RETRO_MEMORY_SAVE_RAM)   return g_sram;
	return NULL;
}

static size_t mem_size(unsigned id)
{
	if (id == RETRO_MEMORY_SYSTEM_RAM) return sizeof g_ram;
	if (id == RETRO_MEMORY_SAVE_RAM)   return sizeof g_sram;
	return 0;
}

static diatom_core g_core;

static void console_reset(void)
{
	memset(g_ram, 0, sizeof g_ram);
	memset(g_sram, 0, sizeof g_sram);
	memset(&g_core, 0, sizeof g_core);
	g_core.get_memory_data = mem_data;
	g_core.get_memory_size = mem_size;

	diatom_cheevos_reset();
	diatom_cheevos_set_console(RC_CONSOLE_NINTENDO);
}

/* ---- 1. no memory map: system RAM, then cartridge RAM --------------------- */

/* The NES' RetroAchievements space is $0000-$FFFF laid out by rc_consoles.h:
 * 2KB of work RAM, three mirrors of it, the PPU and APU registers, cartridge
 * space, 8KB of cartridge RAM at $6000, then ROM. With no map from the core,
 * only the two blocks retro_get_memory_data can return are reachable; the
 * rest has to become holes of exactly the right size, or every address after
 * the first hole is wrong. */
static void test_unmapped(void)
{
	printf("  no memory map, NES:\n");
	console_reset();
	CHECK(diatom_cheevos_resolve(&g_core) == 0x800 + 0x2000,
	      "expected 0x2800 readable bytes");
	CHECK(diatom_cheevos_total_bytes() == 0x10000,
	      "the NES address space is 64KB; got 0x%zx", diatom_cheevos_total_bytes());

	/* work RAM, mirrors, cartridge RAM, ROM */
	CHECK(diatom_cheevos_span_count() == 4,
	      "expected 4 spans; got %d", diatom_cheevos_span_count());

	g_ram[0x6f3]  = 0x5a;
	g_sram[0]     = 0x11;
	g_sram[0x1fff] = 0x22;

	CHECK(diatom_cheevos_peek(0x06f3, 1, NULL) == 0x5a, "work RAM is not at $06f3");
	CHECK(diatom_cheevos_peek(0x6000, 1, NULL) == 0x11, "cartridge RAM is not at $6000");
	CHECK(diatom_cheevos_peek(0x7fff, 1, NULL) == 0x22, "cartridge RAM ends in the wrong place");

	/* $0800 is a mirror of $0000 on real hardware, but the core has not said
	 * so, and guessing would be inventing data. */
	g_ram[0] = 0x99;
	CHECK(diatom_cheevos_peek(0x0800, 1, NULL) == 0,
	      "an unmapped mirror must read as nothing, not as a guess");
	CHECK(diatom_cheevos_peek(0x8000, 1, NULL) == 0, "ROM is not mapped and must read 0");

	/* Little-endian, whatever the console is. */
	g_ram[0x10] = 0x34;
	g_ram[0x11] = 0x12;
	CHECK(diatom_cheevos_peek(0x0010, 2, NULL) == 0x1234, "16-bit read is not little-endian");

	/* A read that runs off the end of a span into a hole is not a short
	 * read: it is no read at all. */
	CHECK(diatom_cheevos_peek(0x07ff, 2, NULL) == 0,
	      "a read spanning into a hole must fail whole");
}

/* ---- 2. with a memory map: mirrors resolve -------------------------------- */

/* What an NES core declares when it describes its bus properly. `select`
 * $E000 claims $0000-$1FFF; `disconnect` $1800 says address lines 11 and 12
 * are not wired to the 2KB chip, which is exactly why the NES mirrors its work
 * RAM four times. */
static const struct retro_memory_descriptor NES_DESC[] = {
	{ RETRO_MEMDESC_SYSTEM_RAM, g_ram, 0, 0x0000, 0xE000, 0x1800, 0x0800, "RAM" },
	{ RETRO_MEMDESC_SAVE_RAM,   g_sram, 0, 0x6000, 0,      0,      0x2000, "SRAM" },
};

static void test_mapped(void)
{
	struct retro_memory_map mm;

	printf("  with a memory map, NES:\n");
	console_reset();
	mm.descriptors = NES_DESC;
	mm.num_descriptors = sizeof NES_DESC / sizeof NES_DESC[0];
	diatom_cheevos_note_map(&mm);

	CHECK(diatom_cheevos_resolve(&g_core) == 0x2000 + 0x2000,
	      "the mirrors should be readable now: 8KB of RAM views plus 8KB of SRAM");
	CHECK(diatom_cheevos_total_bytes() == 0x10000, "still a 64KB address space");

	g_ram[0x6f3] = 0x5a;
	g_ram[0]     = 0x99;

	CHECK(diatom_cheevos_peek(0x06f3, 1, NULL) == 0x5a, "work RAM is not at $06f3");
	CHECK(diatom_cheevos_peek(0x0800, 1, NULL) == 0x99, "mirror at $0800 does not follow $0000");
	CHECK(diatom_cheevos_peek(0x16f3, 1, NULL) == 0x5a, "mirror at $16f3 does not follow $06f3");
	CHECK(diatom_cheevos_peek(0x1ef3, 1, NULL) == 0x5a, "mirror at $1ef3 does not follow $06f3");

	/* The registers at $2000 are outside the descriptor's select, and no
	 * descriptor claims them. */
	CHECK(diatom_cheevos_peek(0x2000, 1, NULL) == 0, "$2000 is hardware, not memory");

	g_sram[0x0100] = 0x77;
	CHECK(diatom_cheevos_peek(0x6100, 1, NULL) == 0x77, "cartridge RAM is not at $6000");
}

/* ---- 3. a real condition, at frame rate ---------------------------------- */

static unsigned g_triggered;

static void on_event(const rc_runtime_event_t *e)
{
	if (e->type == RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED) g_triggered = e->id;
}

/* Blaster Master, achievement 76195, fetched from RetroAchievements
 * 2026-08-29. The last term is the whole argument of ADR-0025: `d0xH06f0` is
 * that byte's value on the PREVIOUS FRAME, so the condition is true only on
 * the single frame the value goes down. A launcher polling a socket at 10Hz
 * against a 60Hz core sees one frame in six and would miss it. */
static const char *BLASTER_MASTER =
	"0xH06f3=0_0xH0400=3_0xH00ba=0_0xH06f0<d0xH06f0";

static void frame(rc_runtime_t *rt)
{
	rc_runtime_do_frame(rt, on_event, diatom_cheevos_peek, NULL, NULL);
}

static void test_condition(void)
{
	rc_runtime_t rt;
	int rc;

	printf("  a real condition, evaluated frame by frame:\n");
	console_reset();
	diatom_cheevos_resolve(&g_core);

	rc_runtime_init(&rt);
	rc = rc_runtime_activate_achievement(&rt, 76195, BLASTER_MASTER, NULL, 0);
	CHECK(rc == RC_OK, "the runtime would not parse the condition: %d", rc);

	g_ram[0x06f3] = 0;
	g_ram[0x0400] = 3;
	g_ram[0x00ba] = 0;
	g_ram[0x06f0] = 5;

	g_triggered = 0;
	frame(&rt);
	CHECK(g_triggered == 0, "fired on the first frame, before there was a previous one");

	frame(&rt);
	CHECK(g_triggered == 0, "fired while the watched byte was unchanged");

	g_ram[0x06f0] = 4;              /* the frame it goes down */
	frame(&rt);
	CHECK(g_triggered == 76195, "did not fire on the frame the value dropped");

	rc_runtime_destroy(&rt);

	/* And the negative: same drop, one guard byte wrong. If this fires, the
	 * one above proves nothing. */
	console_reset();
	diatom_cheevos_resolve(&g_core);
	rc_runtime_init(&rt);
	rc_runtime_activate_achievement(&rt, 76195, BLASTER_MASTER, NULL, 0);

	g_ram[0x06f3] = 0;
	g_ram[0x0400] = 1;              /* wrong: the set wants 3 */
	g_ram[0x00ba] = 0;
	g_ram[0x06f0] = 5;

	g_triggered = 0;
	frame(&rt);
	frame(&rt);
	g_ram[0x06f0] = 4;
	frame(&rt);
	CHECK(g_triggered == 0, "fired with a guard condition unsatisfied");

	rc_runtime_destroy(&rt);
}

int main(void)
{
	printf("cheevos: address space and evaluation\n");
	test_unmapped();
	test_mapped();
	test_condition();

	if (failures) {
		printf("\n%d check(s) failed\n", failures);
		return 1;
	}
	printf("\nok: every check passed\n");
	return 0;
}
