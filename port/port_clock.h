/* Counter ticks to microseconds, for diatom_port_now_us in both ports.
 *
 * Not ticks * 1000000 / freq, which is what both ports did until 2026-09-15.
 * That product overflows 64 bits once ticks passes 2^64 / 10^6, and the clock
 * starts again from zero:
 *
 *   Brick   SDL counts nanoseconds since boot, so it wrapped every 18446.7 s,
 *           5 h 7 min of uptime. Measured 2026-09-15: 4650.5 s against
 *           23097.2 s since boot.
 *   Mac     SDL counts at 24,000,000 a second, so it wrapped every 8.9 days of
 *           uptime. The machine this was measured on had been up 13.
 *
 * Everything timed on this clock inherits the wrap, because a deadline set
 * before it is in the future again after it. Seen on the Brick 2026-09-15: a
 * notice shown for its 3.5 s at 01:38 was drawn over every game after the 02:08
 * wrap, and was still on all three pages over a different game at 04:04. Frame
 * pacing sleeps until a deadline on this clock too, so a game running at the
 * moment of a wrap would sleep a whole cycle - that part is read from the code,
 * not seen.
 *
 * Whole seconds and the remainder are converted separately, which is exact.
 * It can only overflow past 584,000 years of counter, or at a frequency above
 * 1.8e13 ticks a second. */
#ifndef DIATOM_PORT_CLOCK_H
#define DIATOM_PORT_CLOCK_H

#include <stdint.h>

static inline uint64_t diatom_ticks_to_us(uint64_t ticks, uint64_t freq)
{
	return ticks / freq * 1000000ull + ticks % freq * 1000000ull / freq;
}

#endif /* DIATOM_PORT_CLOCK_H */
