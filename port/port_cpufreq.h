/* The CPU's minimum clock, for diatom_port_fast_clock in both Linux ports.
 *
 * Raising the minimum to the maximum holds the top clock under whatever
 * governor is running, without replacing it, and writing the hardware
 * minimum back hands the clock to the governor again. Nothing else on either
 * device sets the minimum, so the hardware's is the one to restore. Policy 0
 * covers every core on both: the Brick's A133 and the Pixel 2's PX30 clock
 * their four cores together.
 *
 * The last state written is kept, so the host can say it every turn and only
 * a change reaches the kernel; the first call always writes, which is how a
 * hold left by a Diatom that crashed is cleared. */
#ifndef DIATOM_PORT_CPUFREQ_H
#define DIATOM_PORT_CPUFREQ_H

#include <stdbool.h>
#include <stdio.h>

#define DIATOM_CPUFREQ "/sys/devices/system/cpu/cpu0/cpufreq/"

static inline bool diatom_cpufreq_copy(const char *from, const char *to)
{
	char v[32];
	FILE *f;
	bool ok;

	f = fopen(from, "r");
	if (!f) return false;
	ok = fgets(v, sizeof v, f) != NULL;
	fclose(f);
	if (!ok) return false;
	f = fopen(to, "w");
	if (!f) return false;
	ok = fputs(v, f) >= 0;
	return fclose(f) == 0 && ok;
}

/* False when the write failed, which leaves the clock as it was. */
static inline bool diatom_cpufreq_hold_top(bool on)
{
	static int held = -1;

	if (held == (int)on) return true;
	held = on;
	return diatom_cpufreq_copy(on ? DIATOM_CPUFREQ "cpuinfo_max_freq"
	                              : DIATOM_CPUFREQ "cpuinfo_min_freq",
	                           DIATOM_CPUFREQ "scaling_min_freq");
}

#endif /* DIATOM_PORT_CPUFREQ_H */
