/* Volume and brightness while a game runs - ADR-0038. See levels.h.
 *
 * Until 2026-10-02 all of this was in the ports, each with its own copy: the
 * positions, the step, the rescale, and - in both of the device ports - the
 * same hold-to-repeat. ADR-0007 says a port deals in pixels, samples, buttons
 * and time, and nothing here is any of those. */
#include "levels.h"

#include <stdint.h>

/* Where each level is, or -1 when it is not known and the hardware has to be
 * asked. */
static int g_pos[DIATOM_LEVEL_COUNT] = { -1, -1 };

void diatom_levels_invalidate(void)
{
	int k;

	for (k = 0; k < DIATOM_LEVEL_COUNT; k++) g_pos[k] = -1;
	diatom_port_level_invalidate();
}

/* ADR-0020's rescale: round-to-nearest, endpoints exact. The endpoints matter
 * most - they are where a user is most likely to sit, and a minimum that drifts
 * off silence after a few round trips is the bug this whole design exists to
 * prevent. */
static int rescale(int index, int from, int to)
{
	if (from <= 1 || to <= 1) return 0;
	if (index < 0)        index = 0;
	if (index > from - 1) index = from - 1;
	return (index * (to - 1) + (from - 1) / 2) / (from - 1);
}

/* The position, read from the hardware the first time after a handover, so the
 * first press moves one step from where the launcher left it. */
static int position(diatom_level_kind kind)
{
	if (g_pos[kind] < 0) g_pos[kind] = diatom_port_level_read(kind);
	return g_pos[kind];
}

bool diatom_levels_get(diatom_level_kind kind, int *index, int *count)
{
	int positions = diatom_port_level_positions(kind);

	if (positions <= 0 || position(kind) < 0) return false;
	*index = g_pos[kind];
	*count = positions;
	return true;
}

bool diatom_levels_set(diatom_level_kind kind, int index, int count)
{
	int positions = diatom_port_level_positions(kind);

	if (positions <= 0) return false;
	g_pos[kind] = rescale(index, count, positions);
	diatom_port_level_write(kind, g_pos[kind]);
	return true;
}

/* One step, written and shown even at an end of the scale, so pressing past
 * the top still shows the bar full. */
static void step(diatom_level_kind kind, int dir)
{
	int positions = diatom_port_level_positions(kind), pos;

	if (positions <= 0 || position(kind) < 0) return;
	pos = g_pos[kind] + dir;
	if (pos < 0)              pos = 0;
	if (pos > positions - 1)  pos = positions - 1;
	g_pos[kind] = pos;
	diatom_port_level_write(kind, pos);
	diatom_port_level_shown(kind, pos, positions);
}

/* Holding a level key repeats it at the launcher's pace - TortOS's
 * REPEAT_DELAY_MS and REPEAT_RATE_MS, 300 and 90 - so a hold does the same thing
 * in a game as at the shelf. */
#define REPEAT_DELAY_US 300000ull
#define REPEAT_RATE_US   90000ull

static const struct {
	uint32_t          key;
	diatom_level_kind kind;
	int               dir;
} level_keys[] = {
	{ DIATOM_LEVEL_KEY_VOLUME_UP,       DIATOM_LEVEL_VOLUME,     +1 },
	{ DIATOM_LEVEL_KEY_VOLUME_DOWN,     DIATOM_LEVEL_VOLUME,     -1 },
	{ DIATOM_LEVEL_KEY_BRIGHTNESS_UP,   DIATOM_LEVEL_BRIGHTNESS, +1 },
	{ DIATOM_LEVEL_KEY_BRIGHTNESS_DOWN, DIATOM_LEVEL_BRIGHTNESS, -1 },
};
#define LEVEL_KEYS (sizeof level_keys / sizeof level_keys[0])

static uint64_t g_next_us[LEVEL_KEYS];   /* when a held key next repeats; 0 is not held */

void diatom_levels_frame(void)
{
	uint32_t held = diatom_port_level_keys();
	uint64_t now = 0;
	size_t i;

	for (i = 0; i < LEVEL_KEYS; i++) {
		if (!(held & level_keys[i].key)) {
			g_next_us[i] = 0;
			continue;
		}
		if (!now) now = diatom_port_now_us();
		if (!g_next_us[i]) {
			g_next_us[i] = now + REPEAT_DELAY_US;
		} else if (now >= g_next_us[i]) {
			g_next_us[i] = now + REPEAT_RATE_US;
		} else {
			continue;
		}
		step(level_keys[i].kind, level_keys[i].dir);
	}
}
