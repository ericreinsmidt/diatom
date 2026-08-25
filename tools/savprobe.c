/* SPIKE: what do our cores actually expose for saving, and what does it cost?
 *
 * Answers, per core:
 *   - retro_get_memory_size for SAVE_RAM / RTC / SYSTEM_RAM
 *   - retro_serialize_size, and whether it moves during a session
 *   - how long retro_serialize takes
 *   - whether SAVE_RAM contents actually change while a game runs
 *
 * An instrument, not Diatom code, and never linked into it. Its numbers are
 * cited by ADR-0016. See tools/README.md.
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

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}

static uint64_t fnv(const void *p, size_t n)
{
	const uint8_t *b = p; uint64_t h = 1469598103934665603ull;
	while (n--) { h ^= *b++; h *= 1099511628211ull; }
	return h;
}

static const char *g_dir = ".";
static unsigned g_quirks;
static bool env(unsigned cmd, void *data)
{
	switch (cmd & 0xffff) {
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY & 0xffff:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY & 0xffff:
		*(const char **)data = g_dir; return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT & 0xffff:   return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE & 0xffff:       *(bool *)data = true; return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE & 0xffff:
		((struct retro_variable *)data)->value = NULL; return false;
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE & 0xffff: *(bool *)data = false; return true;
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION & 0xffff: *(unsigned *)data = 2; return true;
	case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS & 0xffff:
		if (cmd & RETRO_ENVIRONMENT_EXPERIMENTAL) return false;  /* 44 is shared */
		g_quirks = *(unsigned *)data; return true;
	default: return false;
	}
}
static void v_cb(const void *d, unsigned w, unsigned h, size_t p) { (void)d;(void)w;(void)h;(void)p; }
static void a_cb(int16_t l, int16_t r) { (void)l;(void)r; }
static size_t ab_cb(const int16_t *d, size_t f) { (void)d; return f; }
static void p_cb(void) {}
static int16_t s_cb(unsigned a, unsigned b, unsigned c, unsigned d)
{ (void)a;(void)b;(void)c;(void)d; return 0; }

int main(int argc, char **argv)
{
	struct retro_system_info si;
	struct retro_game_info gi;
	size_t sram, rtc, sysram, ss0, ss1;
	uint64_t h0, h1, t;
	void *buf; FILE *f; long len;
	int i;

	if (argc < 3) { fprintf(stderr, "usage: savprobe <core.so> <rom>\n"); return 1; }
	H = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!H) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }

	SYM(void(*)(retro_environment_t), "retro_set_environment")(env);
	SYM(void(*)(retro_video_refresh_t), "retro_set_video_refresh")(v_cb);
	SYM(void(*)(retro_audio_sample_t), "retro_set_audio_sample")(a_cb);
	SYM(void(*)(retro_audio_sample_batch_t), "retro_set_audio_sample_batch")(ab_cb);
	SYM(void(*)(retro_input_poll_t), "retro_set_input_poll")(p_cb);
	SYM(void(*)(retro_input_state_t), "retro_set_input_state")(s_cb);
	SYM(void(*)(void), "retro_init")();
	SYM(void(*)(struct retro_system_info *), "retro_get_system_info")(&si);

	f = fopen(argv[2], "rb");
	if (!f) { perror("rom"); return 3; }
	fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)len);
	if (fread(buf, 1, (size_t)len, f) != (size_t)len) { perror("read"); return 3; }
	fclose(f);

	memset(&gi, 0, sizeof gi);
	gi.path = argv[2]; gi.data = buf; gi.size = (size_t)len;
	if (!SYM(bool(*)(const struct retro_game_info *), "retro_load_game")(&gi)) {
		fprintf(stderr, "load_game failed\n"); return 4;
	}

	sram   = SYM(size_t(*)(unsigned), "retro_get_memory_size")(RETRO_MEMORY_SAVE_RAM);
	rtc    = SYM(size_t(*)(unsigned), "retro_get_memory_size")(RETRO_MEMORY_RTC);
	sysram = SYM(size_t(*)(unsigned), "retro_get_memory_size")(RETRO_MEMORY_SYSTEM_RAM);
	ss0    = SYM(size_t(*)(void), "retro_serialize_size")();

	{
		void *sp = SYM(void *(*)(unsigned), "retro_get_memory_data")(RETRO_MEMORY_SAVE_RAM);
		h0 = sram && sp ? fnv(sp, sram) : 0;
		for (i = 0; i < 600; i++) SYM(void(*)(void), "retro_run")();
		h1 = sram && sp ? fnv(sp, sram) : 0;
	}

	ss1 = SYM(size_t(*)(void), "retro_serialize_size")();

	printf("%-22s %-18s sram %7zu  rtc %5zu  sysram %8zu  state %8zu",
	       si.library_name ? si.library_name : "?",
	       si.library_version ? si.library_version : "?",
	       sram, rtc, sysram, ss1);
	printf("  state %zu -> %zu %s  sram_changed %s  quirks 0x%x",
	       ss0, ss1, ss0 == ss1 ? "stable" : "MOVED",
	       h0 == h1 ? "no" : "YES", g_quirks);

	if (ss1) {
		void *st = malloc(ss1);
		uint64_t best = ~0ull;
		for (i = 0; i < 5; i++) {
			t = now_us();
			SYM(bool(*)(void *, size_t), "retro_serialize")(st, ss1);
			t = now_us() - t;
			if (t < best) best = t;
		}
		printf("  serialize %5llu us", (unsigned long long)best);
		free(st);
	}
	printf("\n");

	SYM(void(*)(void), "retro_unload_game")();
	SYM(void(*)(void), "retro_deinit")();
	free(buf);
	return 0;
}
