/* What does a game launch cost when the process is already up and the core is
 * already resident?
 *
 * The whole case for ADR-0008 (long-lived process) and ADR-0006 (cores stay
 * resident) is that they delete most of a launch. Cold launch is measured at
 * 625-750 ms. The warm number was an assumption until this.
 *
 * Sequence, which is the real one a resident frontend performs:
 *   dlopen + retro_init          ONCE, the cost residency removes
 *   then per game:  read ROM -> retro_load_game -> N frames -> retro_unload_game
 *
 * An instrument, not Diatom code, and never linked into it. See tools/README.md.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libretro.h"

static void *H;
#define SYM(t, n) ((t)dlsym(H, n))

static uint64_t us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000ull + t.tv_nsec / 1000;
}

static const char *g_dir = ".";
static bool env(unsigned cmd, void *data)
{
	switch (cmd & 0xffff) {
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY & 0xffff:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY & 0xffff:
		*(const char **)data = g_dir; return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT & 0xffff:        return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE & 0xffff:            *(bool *)data = true; return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE & 0xffff:
		((struct retro_variable *)data)->value = NULL;       return false;
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE & 0xffff:     *(bool *)data = false; return true;
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION & 0xffff:*(unsigned *)data = 2; return true;
	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE & 0xffff:  *(int *)data = 3; return true;
	default: return false;
	}
}
static void v_cb(const void *d, unsigned w, unsigned h, size_t p){(void)d;(void)w;(void)h;(void)p;}
static void a_cb(int16_t l, int16_t r){(void)l;(void)r;}
static size_t ab_cb(const int16_t *d, size_t f){(void)d; return f;}
static void p_cb(void){}
static int16_t s_cb(unsigned a,unsigned b,unsigned c,unsigned d){(void)a;(void)b;(void)c;(void)d;return 0;}

static void *slurp(const char *path, size_t *len, uint64_t *read_us)
{
	FILE *f; void *buf; long n; uint64_t t0 = us();
	f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)n);
	if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
	fclose(f);
	*len = (size_t)n;
	*read_us = us() - t0;
	return buf;
}

int main(int argc, char **argv)
{
	uint64_t t0, t_dlopen, t_init;
	int i, frames = 60;

	if (argc < 3) { fprintf(stderr, "usage: warmprobe <core.so> <rom>...\n"); return 1; }

	t0 = us();
	H = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	t_dlopen = us() - t0;
	if (!H) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }

	SYM(void(*)(retro_environment_t),        "retro_set_environment")(env);
	SYM(void(*)(retro_video_refresh_t),      "retro_set_video_refresh")(v_cb);
	SYM(void(*)(retro_audio_sample_t),       "retro_set_audio_sample")(a_cb);
	SYM(void(*)(retro_audio_sample_batch_t), "retro_set_audio_sample_batch")(ab_cb);
	SYM(void(*)(retro_input_poll_t),         "retro_set_input_poll")(p_cb);
	SYM(void(*)(retro_input_state_t),        "retro_set_input_state")(s_cb);

	t0 = us();
	SYM(void(*)(void), "retro_init")();
	t_init = us() - t0;

	printf("ONE TIME (what residency removes)\n");
	printf("  dlopen      %7.1f ms\n", t_dlopen / 1000.0);
	printf("  retro_init  %7.1f ms\n", t_init / 1000.0);
	printf("\nPER GAME (what a warm launch actually costs)\n");
	printf("  %-42s %8s %9s %8s %8s\n", "rom", "read", "load_game", "run60", "unload");

	for (i = 2; i < argc; i++) {
		struct retro_game_info gi;
		size_t len; uint64_t rd, t_load, t_run, t_unload;
		void *buf = slurp(argv[i], &len, &rd);
		const char *base = strrchr(argv[i], '/');
		int f;

		if (!buf) { fprintf(stderr, "  cannot read %s\n", argv[i]); continue; }
		memset(&gi, 0, sizeof gi);
		gi.path = argv[i]; gi.data = buf; gi.size = len;

		t0 = us();
		if (!SYM(bool(*)(const struct retro_game_info *), "retro_load_game")(&gi)) {
			fprintf(stderr, "  load_game failed: %s\n", argv[i]);
			free(buf); continue;
		}
		t_load = us() - t0;

		t0 = us();
		for (f = 0; f < frames; f++) SYM(void(*)(void), "retro_run")();
		t_run = us() - t0;

		t0 = us();
		SYM(void(*)(void), "retro_unload_game")();
		t_unload = us() - t0;

		printf("  %-42.42s %6.1fms %7.1fms %6.1fms %6.1fms\n",
		       base ? base + 1 : argv[i],
		       rd / 1000.0, t_load / 1000.0, t_run / 1000.0, t_unload / 1000.0);
		free(buf);
	}

	SYM(void(*)(void), "retro_deinit")();
	return 0;
}
