/* The arithmetic between a level's positions and a device's raw values, for
 * the ports. One copy, each port supplying its own numbers - it was written out
 * in every port until 2026-10-02 (ADR-0038).
 *
 * A WINDOW is a span of raw values with the loudest position at `top` and the
 * quietest at `bottom`, raw growing toward quiet: the Brick's volume register
 * as it is (0 loudest), the GKD Pixel 2's as attenuation from its top (255 - raw).
 * `levels` is the highest position, one less than the number of positions.
 *
 * The same rounding as TortOS's aout_level_to_raw, both ways, so a position
 * read back lands where it was written, and a level that crosses the socket
 * means the same register value on both sides. */
#ifndef DIATOM_PORT_LADDER_H
#define DIATOM_PORT_LADDER_H

#include <stdlib.h>

static inline int ladder_window_raw(int pos, int levels, int top, int bottom)
{
	if (pos < 0)      pos = 0;
	if (pos > levels) pos = levels;
	return top + ((levels - pos) * (bottom - top) + levels / 2) / levels;
}

/* A raw value outside the window was set by something that was not us, and
 * reads as the nearer end of the scale rather than as a position past it. */
static inline int ladder_window_pos(int raw, int levels, int top, int bottom)
{
	int span = bottom - top;

	if (raw >= bottom) return 0;
	if (raw <= top)    return levels;
	return ((bottom - raw) * levels + span / 2) / span;
}

/* The rung nearest `raw`, for a control stepped by a table (brightness), so a
 * level the launcher left between rungs reads as the closest one. */
static inline int ladder_nearest(const unsigned char *rungs, int count, int raw)
{
	int best = 0, i;

	for (i = 1; i < count; i++)
		if (abs(rungs[i] - raw) < abs(rungs[best] - raw)) best = i;
	return best;
}

#endif /* DIATOM_PORT_LADDER_H */
