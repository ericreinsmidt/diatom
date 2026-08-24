/* SPIKE: what does holding every core resident actually cost in RSS, and what
 * does dlopen cost on this CPU?
 *
 * Evaluates ADR-0006's revisit trigger: "measured RSS with all cores mapped and
 * one initialized exceeds ~250 MB".
 *
 * Throwaway. Not Diatom code. See diatom/CLAUDE.md rule 7.
 *
 * usage: rssprobe <rom> <core.so>...     (first core is the one initialized)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <dlfcn.h>
#include "libretro.h"

static long rss_kb(void)
{
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];
	long v = -1;
	if (!f) return -1;
	while (fgets(line, sizeof line, f))
		if (!strncmp(line, "VmRSS:", 6)) { sscanf(line + 6, "%ld", &v); break; }
	fclose(f);
	return v;
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static long base;
static void mark(const char *what, double ms)
{
	long r = rss_kb();
	if (ms >= 0)
		printf("%-34s RSS %6ld kB  (+%5ld)  %7.1f ms\n", what, r, r - base, ms);
	else
		printf("%-34s RSS %6ld kB  (+%5ld)\n", what, r, r - base);
}

static bool env_cb(unsigned cmd, void *data)
{
	switch (cmd & 0xffff) {
	case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char **)data = "."; return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE:
		((struct retro_variable *)data)->value = NULL; return false;
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *)data = false; return true;
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned *)data = 2; return true;
	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: *(int *)data = 3; return true;
	default: return false;
	}
}
static void cb_video(const void *d, unsigned w, unsigned h, size_t p)
{ (void)d; (void)w; (void)h; (void)p; }
static void cb_audio1(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t cb_audio(const int16_t *d, size_t f) { (void)d; return f; }
static void cb_poll(void) { }
static int16_t cb_input(unsigned p, unsigned d, unsigned i, unsigned id)
{ (void)p; (void)d; (void)i; (void)id; return 0; }

int main(int argc, char **argv)
{
	void *h[16];
	int n = 0, i;
	double t0;
	char label[128];
	const char *rom;

	if (argc < 3) { fprintf(stderr, "usage: rssprobe <rom> <core.so>...\n"); return 1; }
	rom = argv[1];

	base = rss_kb();
	printf("baseline (process only)            RSS %6ld kB\n\n", base);

	/* --- every core mapped, never unloaded (ADR-0006) --- */
	for (i = 2; i < argc && n < 16; i++) {
		t0 = now_ms();
		h[n] = dlopen(argv[i], RTLD_NOW | RTLD_LOCAL);   /* ADR-0010 */
		double ms = now_ms() - t0;
		if (!h[n]) { printf("dlopen FAILED %s: %s\n", argv[i], dlerror()); continue; }
		snprintf(label, sizeof label, "dlopen %s", strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i]);
		mark(label, ms);
		n++;
	}
	printf("\n--- %d cores mapped ---\n\n", n);

	if (!n) return 2;

	/* --- initialize exactly one --- */
	{
		void (*p_set_env)(retro_environment_t) = dlsym(h[0], "retro_set_environment");
		void (*p_set_video)(retro_video_refresh_t) = dlsym(h[0], "retro_set_video_refresh");
		void (*p_set_audio)(retro_audio_sample_t) = dlsym(h[0], "retro_set_audio_sample");
		void (*p_set_batch)(retro_audio_sample_batch_t) = dlsym(h[0], "retro_set_audio_sample_batch");
		void (*p_set_poll)(retro_input_poll_t) = dlsym(h[0], "retro_set_input_poll");
		void (*p_set_state)(retro_input_state_t) = dlsym(h[0], "retro_set_input_state");
		void (*p_init)(void) = dlsym(h[0], "retro_init");
		void (*p_info)(struct retro_system_info *) = dlsym(h[0], "retro_get_system_info");
		bool (*p_load)(const struct retro_game_info *) = dlsym(h[0], "retro_load_game");
		void (*p_run)(void) = dlsym(h[0], "retro_run");
		struct retro_system_info si;
		struct retro_game_info gi;
		char *buf = NULL; long len = 0;
		FILE *f;

		p_info(&si);
		p_set_env(env_cb); p_set_video(cb_video); p_set_audio(cb_audio1);
		p_set_batch(cb_audio); p_set_poll(cb_poll); p_set_state(cb_input);

		t0 = now_ms(); p_init(); mark("retro_init (1 core)", now_ms() - t0);

		f = fopen(rom, "rb");
		if (f) {
			fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
			buf = malloc(len);
			if (buf && fread(buf, 1, len, f) != (size_t)len) len = 0;
			fclose(f);
		}
		memset(&gi, 0, sizeof gi);
		gi.path = rom;
		if (!si.need_fullpath) { gi.data = buf; gi.size = len; }

		t0 = now_ms();
		if (!p_load(&gi)) { printf("load_game FAILED\n"); return 3; }
		mark("retro_load_game", now_ms() - t0);

		t0 = now_ms();
		for (i = 0; i < 120; i++) p_run();
		mark("120 x retro_run", now_ms() - t0);

		t0 = now_ms();
		for (i = 0; i < 600; i++) p_run();
		mark("+600 x retro_run (steady state)", now_ms() - t0);
	}

	printf("\n=== VERDICT: %d cores mapped + 1 running = %ld kB (%.1f MB) ===\n",
	       n, rss_kb(), rss_kb() / 1024.0);
	printf("ADR-0006 revisit trigger is 250 MB.\n");
	return 0;
}
