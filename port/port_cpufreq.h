/* The CPU's minimum clock, for diatom_port_fast_clock in both Linux ports.
 *
 * Raising the minimum to the maximum holds the top clock under whatever
 * governor is running, without replacing it, and writing the hardware
 * minimum back hands the clock to the governor again. Nothing else on either
 * device sets the minimum, so the hardware's is the one to restore. Policy 0
 * covers every core on both: the Brick's A133 and the Pixel 2's PX30 clock
 * their four cores together.
 *
 * With boost, the hold also turns on cpufreq boost, which adds the steps the
 * device tree marks turbo-mode (the Pixel 2's 1416 and 1512 MHz), and turns
 * it off again after. Turning boost off doesn't take the top clock back down:
 * the kernel only ever raises cpuinfo_max_freq (cpufreq_frequency_table_cpuinfo
 * in 7.1), so the governor would go on using 1512 MHz at 1x. Releasing the
 * hold sets scaling_max_freq back to the highest step outside boost, and
 * taking it sets it to the top again, as that write outlasts boost.
 *
 * The last state written is kept, so the host can say it every turn and only
 * a change reaches the kernel; the first call always writes, which is how a
 * hold left by a Diatom that crashed is cleared. */
#ifndef DIATOM_PORT_CPUFREQ_H
#define DIATOM_PORT_CPUFREQ_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define DIATOM_CPUFREQ "/sys/devices/system/cpu/cpu0/cpufreq/"
#define DIATOM_CPUFREQ_BOOST "/sys/devices/system/cpu/cpufreq/boost"

static inline bool diatom_cpufreq_write(const char *to, const char *v)
{
	FILE *f = fopen(to, "w");
	bool ok;

	if (!f) return false;
	ok = fputs(v, f) >= 0;
	return fclose(f) == 0 && ok;
}

static inline bool diatom_cpufreq_copy(const char *from, const char *to)
{
	char v[32];
	FILE *f = fopen(from, "r");
	bool ok;

	if (!f) return false;
	ok = fgets(v, sizeof v, f) != NULL;
	fclose(f);
	return ok && diatom_cpufreq_write(to, v);
}

/* The highest step outside boost into scaling_max_freq: the largest in
 * scaling_available_frequencies, which leaves the boost steps out. */
static inline bool diatom_cpufreq_cap_unboosted(void)
{
	char list[256], v[32];
	unsigned long top = 0;
	char *p, *end;
	FILE *f = fopen(DIATOM_CPUFREQ "scaling_available_frequencies", "r");
	bool ok;

	if (!f) return false;
	ok = fgets(list, sizeof list, f) != NULL;
	fclose(f);
	if (!ok) return false;
	for (p = list;; p = end) {
		unsigned long khz = strtoul(p, &end, 10);

		if (end == p) break;
		if (khz > top) top = khz;
	}
	if (!top) return false;
	snprintf(v, sizeof v, "%lu", top);
	return diatom_cpufreq_write(DIATOM_CPUFREQ "scaling_max_freq", v);
}

/* False when a write failed, which may leave the clock where it was. A device
 * with no boost to turn on still holds its top clock without it. */
static inline bool diatom_cpufreq_hold_top(bool on, bool boost)
{
	static int held = -1;
	bool ok = true;

	if (held == (int)on) return true;
	held = on;
	if (!boost)
		return diatom_cpufreq_copy(on ? DIATOM_CPUFREQ "cpuinfo_max_freq"
		                              : DIATOM_CPUFREQ "cpuinfo_min_freq",
		                           DIATOM_CPUFREQ "scaling_min_freq");
	if (on) {
		if (diatom_cpufreq_write(DIATOM_CPUFREQ_BOOST, "1"))
			ok = diatom_cpufreq_copy(DIATOM_CPUFREQ "cpuinfo_max_freq",
			                         DIATOM_CPUFREQ "scaling_max_freq");
		return diatom_cpufreq_copy(DIATOM_CPUFREQ "cpuinfo_max_freq",
		                           DIATOM_CPUFREQ "scaling_min_freq") && ok;
	}
	/* The minimum first, so it is never above the maximum being lowered */
	ok = diatom_cpufreq_copy(DIATOM_CPUFREQ "cpuinfo_min_freq",
	                         DIATOM_CPUFREQ "scaling_min_freq");
	if (diatom_cpufreq_write(DIATOM_CPUFREQ_BOOST, "0"))
		ok = diatom_cpufreq_cap_unboosted() && ok;
	return ok;
}

#endif /* DIATOM_PORT_CPUFREQ_H */
