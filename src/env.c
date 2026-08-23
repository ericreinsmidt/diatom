/* The environment callback.
 *
 * Scope comes from measurement, not guesswork. The env-inventory spike ran six
 * cores through a full lifecycle: of libretro's 77 environment commands, 34
 * appear, ~17 need real answers, 17 are declined by every core with nothing
 * breaking, and 43 never appear at all. See docs/spikes/2026-08-23-env-inventory.md
 *
 * Declining is a legitimate answer and cores handle it. Everything not listed
 * here returns false deliberately.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "diatom.h"

#define MASK(x) ((x) & 0xffff)   /* experimental commands carry 0x10000 */

static diatom_policy    *g_policy;
static diatom_port_caps *g_caps;
static bool              g_geometry_dirty;

bool diatom_env_geometry_changed(void)
{
	bool v = g_geometry_dirty;
	g_geometry_dirty = false;
	return v;
}

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	diatom_log_level l = level >= RETRO_LOG_ERROR ? DIATOM_LOG_ERROR
	                   : level >= RETRO_LOG_WARN  ? DIATOM_LOG_WARN
	                   : DIATOM_LOG_INFO;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);

	buf[strcspn(buf, "\n")] = '\0';
	if (buf[0]) diatom_port_log(l, buf);
}

static bool env_cb(unsigned cmd, void *data)
{
	switch (MASK(cmd)) {

	/* ---- video ---------------------------------------------------------- */
	case MASK(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT): {
		enum retro_pixel_format f = *(const enum retro_pixel_format *)data;
		if (f == RETRO_PIXEL_FORMAT_RGB565)
			g_policy->pixfmt = DIATOM_PIX_RGB565;
		else if (f == RETRO_PIXEL_FORMAT_XRGB8888)
			g_policy->pixfmt = DIATOM_PIX_XRGB8888;
		else
			return false;              /* 0RGB1555 refused — ADR-0007 */
		g_policy->pixfmt_set = true;
		return true;
	}
	case MASK(RETRO_ENVIRONMENT_SET_GEOMETRY):
	case MASK(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO):
		/* Measured: 3 of 6 cores do this mid-run. SNES reports max 604x478
		 * against base 256x224; PC Engine 512x243. Recompute on change. */
		g_geometry_dirty = true;
		return true;
	case MASK(RETRO_ENVIRONMENT_GET_CAN_DUPE):
		*(bool *)data = true;
		return true;
	case MASK(RETRO_ENVIRONMENT_SET_ROTATION):
		/* Declined by default — the port already owns panel rotation, and
		 * honouring this would mean rotation twice over. Logged so we find out
		 * if the assumption is wrong. */
		diatom_port_log(DIATOM_LOG_INFO, "core asked for SET_ROTATION; declined");
		return false;

	/* ---- audio ---------------------------------------------------------- */
	case MASK(RETRO_ENVIRONMENT_GET_TARGET_SAMPLE_RATE):
		/* A lever the spike turned up: a core that asks can generate at the
		 * device rate natively and skip resampling entirely. */
		*(unsigned *)data = (unsigned)g_caps->audio_rate;
		return true;
	case MASK(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE):
		*(int *)data = 3;              /* both enabled; asked every frame */
		return true;

	/* ---- paths — supplied by the host, never by the port ----------------- */
	case MASK(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY):
		*(const char **)data = g_policy->system_dir;
		return g_policy->system_dir != NULL;
	case MASK(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY):
		*(const char **)data = g_policy->save_dir;
		return g_policy->save_dir != NULL;

	/* ---- core options ---------------------------------------------------- */
	case MASK(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION):
		*(unsigned *)data = 2;
		return true;
	case MASK(RETRO_ENVIRONMENT_SET_VARIABLES):
	case MASK(RETRO_ENVIRONMENT_SET_CORE_OPTIONS):
	case MASK(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL):
	case MASK(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2):
	case MASK(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL):
		/* V2_INTL is the path all six measured cores actually use. Accepted
		 * and ignored: options are not surfaced yet, so cores use defaults. */
		return true;
	case MASK(RETRO_ENVIRONMENT_GET_VARIABLE):
		((struct retro_variable *)data)->value = NULL;
		return false;                  /* no value set → core keeps its default */
	case MASK(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE):
		/* Every frame, all six cores. Must stay this cheap. */
		*(bool *)data = false;
		return true;

	/* ---- accepted and ignored ------------------------------------------- */
	case MASK(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL):
	case MASK(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS):
	case MASK(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO):
	case MASK(RETRO_ENVIRONMENT_SET_MEMORY_MAPS):
	case MASK(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS):
		return true;

	case MASK(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME):
		/* Recorded rather than ignored: such a core is loaded with
		 * retro_load_game(NULL), and passing it a path instead fails. */
		g_policy->supports_no_game = data ? *(const bool *)data : true;
		return true;

	/* ---- misc ------------------------------------------------------------ */
	case MASK(RETRO_ENVIRONMENT_GET_LOG_INTERFACE):
		((struct retro_log_callback *)data)->log = core_log;
		return true;
	case MASK(RETRO_ENVIRONMENT_GET_LANGUAGE):
		*(unsigned *)data = RETRO_LANGUAGE_ENGLISH;
		return true;

	default:
		/* Declined on purpose. The 17 commands cores ask for and shrug off
		 * when refused are listed in the spike; anything new lands here too. */
		return false;
	}
}

static void cb_input_poll(void)
{
	diatom_port_input_poll();
}

/* Canonical Diatom buttons -> retropad. Near-identity by design; its purpose is
 * keeping libretro.h out of the port, not translation. */
static const int button_map[DIATOM_BTN_COUNT] = {
	[DIATOM_BTN_UP]     = RETRO_DEVICE_ID_JOYPAD_UP,
	[DIATOM_BTN_DOWN]   = RETRO_DEVICE_ID_JOYPAD_DOWN,
	[DIATOM_BTN_LEFT]   = RETRO_DEVICE_ID_JOYPAD_LEFT,
	[DIATOM_BTN_RIGHT]  = RETRO_DEVICE_ID_JOYPAD_RIGHT,
	[DIATOM_BTN_A]      = RETRO_DEVICE_ID_JOYPAD_A,
	[DIATOM_BTN_B]      = RETRO_DEVICE_ID_JOYPAD_B,
	[DIATOM_BTN_X]      = RETRO_DEVICE_ID_JOYPAD_X,
	[DIATOM_BTN_Y]      = RETRO_DEVICE_ID_JOYPAD_Y,
	[DIATOM_BTN_L1]     = RETRO_DEVICE_ID_JOYPAD_L,
	[DIATOM_BTN_R1]     = RETRO_DEVICE_ID_JOYPAD_R,
	[DIATOM_BTN_L2]     = RETRO_DEVICE_ID_JOYPAD_L2,
	[DIATOM_BTN_R2]     = RETRO_DEVICE_ID_JOYPAD_R2,
	[DIATOM_BTN_SELECT] = RETRO_DEVICE_ID_JOYPAD_SELECT,
	[DIATOM_BTN_START]  = RETRO_DEVICE_ID_JOYPAD_START,
	[DIATOM_BTN_MENU]   = -1,          /* Diatom's own; never reaches a core */
};

static int16_t cb_input_state(unsigned port, unsigned device,
                              unsigned index, unsigned id)
{
	uint32_t state;
	int b;

	(void)index;
	if (port != 0 || device != RETRO_DEVICE_JOYPAD) return 0;

	state = diatom_port_input_state();
	for (b = 0; b < DIATOM_BTN_COUNT; b++)
		if (button_map[b] == (int)id)
			return (state & DIATOM_BIT(b)) ? 1 : 0;
	return 0;
}

/* Video and audio callbacks live in main.c, which owns the frame buffer and
 * the pacing decision — ADR-0007 has the frame consumed after retro_run
 * returns, not from inside the callback. */
extern void diatom_on_video(const void *data, unsigned w, unsigned h, size_t pitch);
extern void diatom_on_audio_batch_store(const int16_t *data, size_t frames);

static void cb_video(const void *data, unsigned w, unsigned h, size_t pitch)
{
	diatom_on_video(data, w, h, pitch);
}
static size_t cb_audio_batch(const int16_t *data, size_t frames)
{
	diatom_on_audio_batch_store(data, frames);
	return frames;
}
static void cb_audio_sample(int16_t l, int16_t r)
{
	int16_t f[2] = { l, r };
	diatom_on_audio_batch_store(f, 1);
}

void diatom_env_bind(diatom_core *c, diatom_policy *p, diatom_port_caps *caps)
{
	g_policy = p;
	g_caps   = caps;

	c->set_environment(env_cb);
	c->set_video_refresh(cb_video);
	c->set_audio_sample(cb_audio_sample);
	c->set_audio_sample_batch(cb_audio_batch);
	c->set_input_poll(cb_input_poll);
	c->set_input_state(cb_input_state);
}
