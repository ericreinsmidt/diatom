/* SPDX-License-Identifier: MIT */
/* What a first launch pays, per core, measured rather than remembered.
 *
 * dlopen and retro_init are separate decisions and this times them separately.
 * Mapping a core holds file-backed pages the kernel can evict; retro_init
 * leaves it holding live state. Preloading the first at boot is cheap,
 * preloading the second is a commitment, and if init is where the time goes
 * then mapping alone buys almost nothing - which is exactly the question.
 *
 * The only figures anyone had were 6 ms and 43 ms for FCEUmm, in core.c, and
 * FCEUmm is the small one. The README says dlopen costs ~170 ms, which is
 * from the old process-per-game path and cannot be true of a warm one.
 *
 * Creates no renderer and presents nothing - ADR-0001 - so it is safe to run
 * beside the launcher, which is the whole reason it is a probe rather than a
 * flag on diatom.
 *
 *   tools/coreprobe /path/to/a_libretro.so [more...]
 */
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

/* Refuses everything. A core may ask for anything during retro_init; saying no
 * to all of it is what a frontend that has not decided yet would do, and any
 * core that cannot survive that would not survive Diatom's startup either. */
static bool env_cb(unsigned cmd, void *data) { (void)cmd; (void)data; return false; }

static const char *base(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

int main(int argc, char **argv)
{
	int i;

	if (argc < 2) {
		fprintf(stderr, "usage: coreprobe <core.so> [core.so ...]\n");
		return 2;
	}

	printf("%-30s %10s %12s\n", "core", "dlopen", "retro_init");
	for (i = 1; i < argc; i++) {
		void (*set_env)(bool (*)(unsigned, void *));
		void (*init)(void);
		double t0, dl, in;
		void *h;

		t0 = now_ms();
		h  = dlopen(argv[i], RTLD_NOW | RTLD_LOCAL);
		dl = now_ms() - t0;
		if (!h) {
			printf("%-30s  dlopen failed: %s\n", base(argv[i]), dlerror());
			continue;
		}

		*(void **)&set_env = dlsym(h, "retro_set_environment");
		*(void **)&init    = dlsym(h, "retro_init");
		if (!set_env || !init) {
			printf("%-30s  missing retro_* symbols\n", base(argv[i]));
			continue;
		}

		/* Before init, per the libretro spec: a core is entitled to ask
		 * things of the frontend while initialising. */
		set_env(env_cb);
		t0 = now_ms();
		init();
		in = now_ms() - t0;

		printf("%-30s %7.1f ms %9.1f ms\n", base(argv[i]), dl, in);
	}
	return 0;
}
