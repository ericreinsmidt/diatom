/* SPDX-License-Identifier: MIT */
/* How fast can each core go? The ceiling for fast forward, per core.
 *
 * Fast forward on the Brick means running several core frames for each one
 * presented: the flip is vsync-latched (ADR-0013), so presenting faster is not
 * available. What sets the speed is therefore retro_run alone, and this times
 * it with nothing in the way - no pacing, no present, no audio device.
 *
 * Loads a core, a ROM (unzipped: this has no zip reader) and optionally a
 * Diatom state, so the frames measured are gameplay rather than a title
 * screen. Runs a warmup, so the cpufreq governor has ramped, then times each
 * of `frames` frames and reports the sustained speed as a multiple of the
 * core's own rate, and the per-frame times that decide whether a speed holds
 * through the heavy moments and not only on average.
 *
 * The video callback copies the frame, as Diatom's does; audio is counted and
 * thrown away. Presents nothing and opens no device, so it is safe beside an
 * idle launcher - never beside a running game, whose CPU it would take.
 *
 *   tools/ffprobe <core.so> <rom> [state] [frames]
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

/* Diatom's state header, from src/save.c: 8 + 4 + 4 + 64 + 32 bytes. */
#define STATE_MAGIC  "DIATOMST"
#define STATE_HEADER 112

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}

/* Beetle's CD loader logs through this without checking it was given one. */
static void log_cb(enum retro_log_level level, const char *fmt, ...) { (void)level; (void)fmt; }

static const char *g_dir = "/mnt/SDCARD/Bios";
static bool env(unsigned cmd, void *data)
{
	switch (cmd & 0xffff) {
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS & 0xffff:
		/* FFPROBE_INPUTS=1: which buttons the core says it uses. */
		if (getenv("FFPROBE_INPUTS")) {
			const struct retro_input_descriptor *d = data;

			for (; d && d->description; d++)
				if (d->port == 0 && d->device == RETRO_DEVICE_JOYPAD)
					fprintf(stderr, "  input %2u %s\n", d->id, d->description);
		}
		return true;
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS & 0xffff:
		return false;    /* so every read is one id, and can be counted */
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE & 0xffff:
		((struct retro_log_callback *)data)->log = log_cb; return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY & 0xffff:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY & 0xffff:
		*(const char **)data = g_dir; return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT & 0xffff:   return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE & 0xffff:       *(bool *)data = true; return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE & 0xffff:
		((struct retro_variable *)data)->value = NULL; return false;
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE & 0xffff: *(bool *)data = false; return true;
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION & 0xffff: *(unsigned *)data = 2; return true;
	default: return false;
	}
}

/* Big enough for any of the six cores' largest frame at 32 bits. */
static uint8_t g_frame[1024 * 1024 * 4];
static size_t  g_audio;

static void v_cb(const void *d, unsigned w, unsigned h, size_t pitch)
{
	(void)w;
	if (d && (size_t)h * pitch <= sizeof g_frame) memcpy(g_frame, d, (size_t)h * pitch);
}
static void a_cb(int16_t l, int16_t r) { (void)l; (void)r; g_audio++; }
static size_t ab_cb(const int16_t *d, size_t f) { (void)d; g_audio += f; return f; }
static void p_cb(void) {}
/* Reads of each joypad id on port 0, for FFPROBE_INPUTS. */
static unsigned long g_reads[16];
static int16_t s_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
	(void)index;
	if (port == 0 && device == RETRO_DEVICE_JOYPAD && id < 16) g_reads[id]++;
	return 0;
}

static void *slurp(const char *path, long *len)
{
	FILE *f = fopen(path, "rb");
	void *buf;

	if (!f) return NULL;
	fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)*len);
	if (!buf || fread(buf, 1, (size_t)*len, f) != (size_t)*len) { fclose(f); free(buf); return NULL; }
	fclose(f);
	return buf;
}

static long cpu_mhz(void)
{
	FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
	long khz = 0;

	if (f) { if (fscanf(f, "%ld", &khz) != 1) khz = 0; fclose(f); }
	return khz / 1000;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
	struct retro_system_info si;
	struct retro_system_av_info av;
	struct retro_game_info gi;
	const char *rom, *base;
	uint64_t *ft, t0, total;
	long len, mhz_mid = 0;
	void *buf;
	int frames = 1800, warm = 120, i;
	double fps, speed;

	if (argc < 3) { fprintf(stderr, "usage: ffprobe <core.so> <rom> [state] [frames]\n"); return 1; }
	if (argc > 4) frames = atoi(argv[4]);
	if (frames < 60) frames = 60;
	rom = argv[2];
	base = strrchr(rom, '/') ? strrchr(rom, '/') + 1 : rom;

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

	memset(&gi, 0, sizeof gi);
	gi.path = rom;
	if (!si.need_fullpath) {
		if (!(buf = slurp(rom, &len))) { perror("rom"); return 3; }
		gi.data = buf; gi.size = (size_t)len;
	}
	if (!SYM(bool(*)(const struct retro_game_info *), "retro_load_game")(&gi)) {
		fprintf(stderr, "load_game failed\n"); return 4;
	}
	SYM(void(*)(struct retro_system_av_info *), "retro_get_system_av_info")(&av);

	/* One frame first: some cores finish setting up inside their first run,
	 * and a state restored before it can be undone by it. */
	SYM(void(*)(void), "retro_run")();
	if (argc > 3 && argv[3][0]) {
		uint8_t *st = slurp(argv[3], &len);

		if (!st || len <= STATE_HEADER || memcmp(st, STATE_MAGIC, 8) != 0) {
			fprintf(stderr, "%s: not a Diatom state\n", argv[3]); return 5;
		}
		if (!SYM(bool(*)(const void *, size_t), "retro_unserialize")(st + STATE_HEADER,
		                                                           (size_t)(len - STATE_HEADER))) {
			fprintf(stderr, "unserialize failed\n"); return 5;
		}
		free(st);
	}

	for (i = 0; i < warm; i++) SYM(void(*)(void), "retro_run")();

	ft = malloc(sizeof *ft * (size_t)frames);
	g_audio = 0;
	t0 = now_us();
	for (i = 0; i < frames; i++) {
		uint64_t s = now_us();

		SYM(void(*)(void), "retro_run")();
		ft[i] = now_us() - s;
		if (i == frames / 2) mhz_mid = cpu_mhz();
	}
	total = now_us() - t0;

	fps   = frames * 1e6 / (double)total;
	speed = fps / av.timing.fps;
	qsort(ft, (size_t)frames, sizeof *ft, cmp_u64);
	printf("%-18s %-40.40s %6.2f fps native  %7.1f fps  %5.2fx  "
	       "frame p50 %5.2f  p95 %5.2f  max %6.2f ms  (%.2fx at p95)  %ld MHz  %.0f samples/frame\n",
	       si.library_name ? si.library_name : "?", base, av.timing.fps, fps, speed,
	       ft[frames / 2] / 1000.0, ft[frames * 95 / 100] / 1000.0, ft[frames - 1] / 1000.0,
	       1e6 / av.timing.fps / (double)ft[frames * 95 / 100], mhz_mid,
	       (double)g_audio / frames);

	if (getenv("FFPROBE_INPUTS")) {
		static const char *const name[16] = { "B", "Y", "SELECT", "START", "UP", "DOWN",
			"LEFT", "RIGHT", "A", "X", "L", "R", "L2", "R2", "L3", "R3" };

		fprintf(stderr, "  reads over %d frames:", warm + frames);
		for (i = 0; i < 16; i++) fprintf(stderr, " %s %lu", name[i], g_reads[i]);
		fprintf(stderr, "\n");
	}

	SYM(void(*)(void), "retro_unload_game")();
	SYM(void(*)(void), "retro_deinit")();
	return 0;
}
