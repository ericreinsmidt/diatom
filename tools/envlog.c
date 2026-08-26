/* SPIKE: which RETRO_ENVIRONMENT_* calls do our cores actually make, and at
 * which lifecycle phase?
 *
 * An instrument, not Diatom code, and never linked into it. See
 * docs/working-agreement.md practice 7 and tools/README.md.
 *
 * Answers the minimum needed for a core to get going and declines everything
 * else -- a decline is still a data point, which is the whole purpose.
 *
 * usage: envlog <core.so> [rom] [frames]
 *
 * `frames` defaults to 120. Raise it when the question is WHEN something
 * happens rather than whether: ADR-0011 locks the display rect from the
 * geometry reported at load, which assumes that geometry is representative.
 * Cores that boot into one mode and switch to another do it some way into the
 * run, so a 120-frame window can miss the change entirely and report a core as
 * stable when it is not.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <dlfcn.h>
#include "libretro.h"
#include "env_names.h"   /* generated; see tools/gen_env_names.py */

/* Geometry timeline: every distinct frame size the core emits, and the frame it
 * first appeared on. This is what ADR-0011's assumption stands or falls on, and
 * a count of SET_GEOMETRY calls cannot answer it - a core can change the size of
 * the frames it delivers without announcing anything. */
#define MAXGEO 32
static struct { unsigned w, h; long first, count; } geo[MAXGEO];
static int  n_geo;
static long frame_no;

static void note_geometry(unsigned w, unsigned h)
{
	int i;
	for (i = 0; i < n_geo; i++)
		if (geo[i].w == w && geo[i].h == h) { geo[i].count++; return; }
	if (n_geo < MAXGEO) {
		geo[n_geo].w = w; geo[n_geo].h = h;
		geo[n_geo].first = frame_no; geo[n_geo].count = 1;
		n_geo++;
	}
}

#define MAXCMD 256
#define M(x) ((x) & 0xffff)
enum { P_SETENV, P_INIT, P_LOAD, P_RUN, P_UNLOAD, P_DEINIT, P_N };
static const char *phase_name[P_N] = {
	"set_environment", "init", "load_game", "run", "unload", "deinit"
};

static int phase = P_SETENV;
static unsigned hits[MAXCMD][P_N];
static unsigned declined[MAXCMD];
/* Two commands can share a number and differ only by the experimental bit
 * (44 is both SET_SERIALIZATION_QUIRKS and SET_HW_SHARED_CONTEXT), so the
 * bit has to be kept to name either of them correctly. */
static unsigned exp_bit[MAXCMD];
static enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static unsigned n_core_options;

static void logger(enum retro_log_level lvl, const char *fmt, ...) { (void)lvl; (void)fmt; }

static bool env_cb(unsigned cmd, void *data)
{
	unsigned n = cmd & 0xffff;
	bool answered = true;

	if (n < MAXCMD) {
		hits[n][phase]++;
		exp_bit[n] = cmd & 0x10000;
	}

	switch (n) {
	case M(RETRO_ENVIRONMENT_GET_CAN_DUPE):
		*(bool *)data = true; break;
	case M(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT):
		pixfmt = *(const enum retro_pixel_format *)data; break;
	case M(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY):
	case M(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY):
	case M(RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY):
		*(const char **)data = "."; break;
	case M(RETRO_ENVIRONMENT_GET_LOG_INTERFACE):
		((struct retro_log_callback *)data)->log = logger; break;
	case M(RETRO_ENVIRONMENT_GET_VARIABLE):
		((struct retro_variable *)data)->value = NULL; answered = false; break;
	case M(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE):
		*(bool *)data = false; break;
	case M(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION):
		*(unsigned *)data = 2; break;
	case M(RETRO_ENVIRONMENT_SET_VARIABLES): {
		const struct retro_variable *v = data;
		while (v && v->key) { n_core_options++; v++; }
		break;
	}
	case M(RETRO_ENVIRONMENT_SET_CORE_OPTIONS): {
		const struct retro_core_option_definition *d = data;
		while (d && d->key) { n_core_options++; d++; }
		break;
	}
	case M(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2): {
		const struct retro_core_options_v2 *v2 = data;
		const struct retro_core_option_v2_definition *d = v2 ? v2->definitions : NULL;
		while (d && d->key) { n_core_options++; d++; }
		break;
	}
	case M(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL): {
		const struct retro_core_options_intl *in = data;
		const struct retro_core_option_definition *d = in ? in->us : NULL;
		while (d && d->key) { n_core_options++; d++; }
		break;
	}
	case M(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL): {
		const struct retro_core_options_v2_intl *in = data;
		const struct retro_core_option_v2_definition *d =
			(in && in->us) ? in->us->definitions : NULL;
		while (d && d->key) { n_core_options++; d++; }
		break;
	}
	case M(RETRO_ENVIRONMENT_GET_LANGUAGE):
		*(unsigned *)data = RETRO_LANGUAGE_ENGLISH; break;
	case M(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE):
		*(int *)data = 3; break;
	case M(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS):
		answered = false; break;   /* decline: exercise per-button path */
	case M(RETRO_ENVIRONMENT_SET_GEOMETRY): {
		const struct retro_game_geometry *g = data;
		if (g)
			printf("  SET_GEOMETRY at frame %ld: base %ux%u max %ux%u aspect %.4f\n",
			       frame_no, g->base_width, g->base_height,
			       g->max_width, g->max_height, (double)g->aspect_ratio);
		break;
	}
	case M(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO): {
		/* Logged separately from SET_GEOMETRY because a core may use either,
		 * and reading only one of them makes a core look stable when it is
		 * not. mednafen_pce_fast uses both, with different values. */
		const struct retro_system_av_info *a = data;
		if (a)
			printf("  SET_SYSTEM_AV_INFO at frame %ld: base %ux%u max %ux%u "
			       "aspect %.4f fps %.4f rate %.1f\n",
			       frame_no, a->geometry.base_width, a->geometry.base_height,
			       a->geometry.max_width, a->geometry.max_height,
			       (double)a->geometry.aspect_ratio,
			       a->timing.fps, a->timing.sample_rate);
		break;
	}
	case M(RETRO_ENVIRONMENT_SET_MESSAGE):
	case M(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS):
	case M(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL):
	case M(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS):
	case M(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO):
	case M(RETRO_ENVIRONMENT_SET_MEMORY_MAPS):
	case M(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME):
		break;
	default:
		answered = false; break;
	}

	if (!answered && n < MAXCMD) declined[n]++;
	return answered;
}

static void cb_video(const void *d, unsigned w, unsigned h, size_t p)
{
	(void)p;
	/* d == NULL is a duplicate-frame signal and carries no size. */
	if (d) note_geometry(w, h);
}
static void cb_audio1(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t cb_audio(const int16_t *d, size_t f) { (void)d; return f; }
static void cb_poll(void) { }
static int16_t cb_input(unsigned p, unsigned d, unsigned i, unsigned id)
{ (void)p; (void)d; (void)i; (void)id; return 0; }

#define SYM(v, n) do { \
	*(void **)(&v) = dlsym(h, n); \
	if (!v) { fprintf(stderr, "missing %s\n", n); return 2; } \
} while (0)

int main(int argc, char **argv)
{
	void *h;
	void (*p_set_env)(retro_environment_t);
	int frames;
	void (*p_set_video)(retro_video_refresh_t);
	void (*p_set_audio)(retro_audio_sample_t);
	void (*p_set_audio_batch)(retro_audio_sample_batch_t);
	void (*p_set_poll)(retro_input_poll_t);
	void (*p_set_state)(retro_input_state_t);
	void (*p_init)(void);
	void (*p_deinit)(void);
	void (*p_get_info)(struct retro_system_info *);
	void (*p_get_av)(struct retro_system_av_info *);
	bool (*p_load)(const struct retro_game_info *);
	void (*p_unload)(void);
	void (*p_run)(void);
	struct retro_system_info si;
	struct retro_system_av_info av;
	struct retro_game_info gi;
	char *rombuf = NULL;
	long romlen = 0;
	int i, loaded = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: envlog <core.so> [rom] [frames]\n");
		return 1;
	}
	frames = argc > 3 ? atoi(argv[3]) : 120;

	h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }

	SYM(p_set_env, "retro_set_environment");
	SYM(p_set_video, "retro_set_video_refresh");
	SYM(p_set_audio, "retro_set_audio_sample");
	SYM(p_set_audio_batch, "retro_set_audio_sample_batch");
	SYM(p_set_poll, "retro_set_input_poll");
	SYM(p_set_state, "retro_set_input_state");
	SYM(p_init, "retro_init");
	SYM(p_deinit, "retro_deinit");
	SYM(p_get_info, "retro_get_system_info");
	SYM(p_get_av, "retro_get_system_av_info");
	SYM(p_load, "retro_load_game");
	SYM(p_unload, "retro_unload_game");
	SYM(p_run, "retro_run");

	p_get_info(&si);
	printf("### core: %s\n", argv[1]);
	printf("name=%s version=%s ext=%s need_fullpath=%d block_extract=%d\n",
	       si.library_name ? si.library_name : "?",
	       si.library_version ? si.library_version : "?",
	       si.valid_extensions ? si.valid_extensions : "?",
	       si.need_fullpath, si.block_extract);

	phase = P_SETENV;  p_set_env(env_cb);
	p_set_video(cb_video); p_set_audio(cb_audio1);
	p_set_audio_batch(cb_audio); p_set_poll(cb_poll); p_set_state(cb_input);

	phase = P_INIT;    p_init();

	if (argc > 2) {
		FILE *f = fopen(argv[2], "rb");
		if (f) {
			fseek(f, 0, SEEK_END); romlen = ftell(f); fseek(f, 0, SEEK_SET);
			rombuf = malloc(romlen);
			if (rombuf && fread(rombuf, 1, romlen, f) != (size_t)romlen) romlen = 0;
			fclose(f);
		}
		memset(&gi, 0, sizeof gi);
		gi.path = argv[2];
		if (!si.need_fullpath) { gi.data = rombuf; gi.size = romlen; }
		phase = P_LOAD;
		loaded = p_load(&gi) ? 1 : 0;
		printf("load_game: %s\n", loaded ? "ok" : "FAILED");
		if (loaded) {
			p_get_av(&av);
			printf("av: %ux%u max %ux%u fps=%.4f rate=%.1f\n",
			       av.geometry.base_width, av.geometry.base_height,
			       av.geometry.max_width, av.geometry.max_height,
			       av.timing.fps, av.timing.sample_rate);
			phase = P_RUN;
			for (i = 0; i < frames; i++) { frame_no = i; p_run(); }
			phase = P_UNLOAD; p_unload();
		}
	}

	phase = P_DEINIT; p_deinit();

	printf("pixel_format=%s core_options=%u\n",
	       pixfmt == RETRO_PIXEL_FORMAT_RGB565 ? "RGB565" :
	       pixfmt == RETRO_PIXEL_FORMAT_XRGB8888 ? "XRGB8888" : "0RGB1555",
	       n_core_options);
	printf("%3s %-38s %-6s %s\n", "CMD", "ENVIRONMENT CALL", "TOTAL", "PHASES (declined)");
	for (i = 0; i < MAXCMD; i++) {
		unsigned tot = 0, p;
		for (p = 0; p < P_N; p++) tot += hits[i][p];
		if (!tot) continue;
		printf("%3d %-38s %-6u", i, diatom_env_name((unsigned)i | exp_bit[i]), tot);
		for (p = 0; p < P_N; p++)
			if (hits[i][p]) printf(" %s:%u", phase_name[p], hits[i][p]);
		if (declined[i]) printf("  DECLINED:%u", declined[i]);
		if (exp_bit[i]) printf("  [exp]");
		printf("\n");
	}
	if (n_geo) {
		int i;
		printf("\ngeometry timeline (%d distinct frame size(s) over %d frames)\n",
		       n_geo, frames);
		for (i = 0; i < n_geo; i++)
			printf("  %ux%-10u first seen frame %-6ld %ld frame(s)\n",
			       geo[i].w, geo[i].h, geo[i].first, geo[i].count);
		if (n_geo > 1)
			printf("  NOTE: the size at load is not the only size this core "
			       "emits - ADR-0011 locks the rect from the first.\n");
	}
	printf("### end %s\n\n", argv[1]);
	free(rombuf);
	return 0;
}
