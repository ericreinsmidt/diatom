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
#include <stdlib.h>
#include <string.h>

#include "cheevos.h"
#include "diatom.h"

#define MASK(x) ((x) & 0xffff)   /* experimental commands carry 0x10000 */

static void capture_descriptors(const struct retro_input_descriptor *d);

static diatom_policy    *g_policy;
static diatom_port_caps *g_caps;
static bool              g_geometry_dirty;
static int               g_new_w, g_new_h;
static double            g_new_aspect;
static uint32_t          g_suppress;

void diatom_env_suppress(uint32_t mask)
{
	g_suppress = mask;
}

bool diatom_env_geometry_changed(void)
{
	bool v = g_geometry_dirty;
	g_geometry_dirty = false;
	return v;
}

/* What the core changed it TO. Only meaningful straight after
 * diatom_env_geometry_changed() returned true. */
bool diatom_env_new_geometry(int *w, int *h, double *aspect)
{
	if (g_new_w <= 0 || g_new_h <= 0) return false;
	*w = g_new_w;
	*h = g_new_h;
	*aspect = g_new_aspect > 0.0 ? g_new_aspect
	                             : (double)g_new_w / (double)g_new_h;
	return true;
}

/* Raised once the game is running. Cores are free to be as chatty as they like
 * at DEBUG/INFO and some are extravagant: mGBA logs EVERY DMA transfer at info,
 * measured at 5.2 lines per frame on Ninja Five-0.
 *
 * That is not a tidiness problem. Under the launcher the host's stdout is a
 * file on the SD card, and the cost of writing it wrecks the frame budget.
 * Measured 2026-08-28, same ROM, same display mode, only the destination
 * changed:
 *
 *   output discarded    59.73 fps    0 resyncs   0 dropped   queue min 445
 *   output to the card  43.18 fps   68 resyncs  24 dropped   queue min   0
 *
 * A queue minimum of zero is a real underrun, which is an audible gap, and it
 * was reported as the game and its audio stuttering badly. Standalone testing
 * never saw it because a pipe is cheap and an SD card is not.
 *
 * Load-time INFO is kept - that is where a core announces its version, which is
 * worth having in a log. Only the per-frame flood is dropped, and
 * DIATOM_CORE_LOG=1 keeps everything for anyone debugging a core. */
static bool g_core_log_quiet;

void diatom_env_core_log_quiet(bool quiet)
{
	static int forced = -1;
	if (forced < 0) {
		const char *e = getenv("DIATOM_CORE_LOG");
		forced = (e && *e && *e != '0') ? 1 : 0;
	}
	g_core_log_quiet = forced ? false : quiet;
}

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	diatom_log_level l = level >= RETRO_LOG_ERROR ? DIATOM_LOG_ERROR
	                   : level >= RETRO_LOG_WARN  ? DIATOM_LOG_WARN
	                   : DIATOM_LOG_INFO;

	if (g_core_log_quiet && level < RETRO_LOG_WARN) return;

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
			return false;              /* 0RGB1555 refused - ADR-0007 */
		g_policy->pixfmt_set = true;
		return true;
	}
	case MASK(RETRO_ENVIRONMENT_SET_GEOMETRY):
	case MASK(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO): {
		/* The new geometry is CARRIED now, not just flagged.
		 *
		 * ADR-0011 locked the rect from load-time geometry on the theory that a
		 * mid-run change is a hires excursion. Measured 2026-08-26, that is
		 * false for Genesis: genesis_plus_gx reports 256x192 at load and
		 * switches to 320x224 at frame 1 (Phantasy Star IV) or 29 (Herzog
		 * Zwei), then stays there. The load-time value is a boot artifact and
		 * the caller cannot tell without seeing what replaced it. */
		const struct retro_game_geometry *g = data;

		if (MASK(cmd) == MASK(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO) && data)
			g = &((const struct retro_system_av_info *)data)->geometry;
		if (g && g->base_width && g->base_height) {
			g_new_w      = (int)g->base_width;
			g_new_h      = (int)g->base_height;
			g_new_aspect = (double)g->aspect_ratio;
		}
		g_geometry_dirty = true;
		return true;
	}
	case MASK(RETRO_ENVIRONMENT_GET_CAN_DUPE):
		*(bool *)data = true;
		return true;
	case MASK(RETRO_ENVIRONMENT_SET_ROTATION):
		/* Declined by default - the port already owns panel rotation, and
		 * honoring this would mean rotation twice over. Logged so we find out
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

	/* ---- paths - supplied by the host, never by the port ----------------- */
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
	case MASK(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL):
		/* The path all six measured cores actually use. `us` is the English
		 * set; `local` is a translation of the same keys, so the values we
		 * serve are identical either way. */
		if (data) {
			const struct retro_core_options_v2_intl *in = data;
			diatom_options_define_v2(in->us ? in->us : in->local);
		}
		return true;
	case MASK(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2):
		diatom_options_define_v2(data);
		return true;
	case MASK(RETRO_ENVIRONMENT_SET_CORE_OPTIONS):
	case MASK(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL):
		/* v1 carries retro_core_option_definition, which differs from v2 only
		 * by the category fields we do not use. Not built: no core in the
		 * matrix uses it, and untested code that silently mis-parses options
		 * is worse than a core falling back to its own defaults. */
		return true;
	case MASK(RETRO_ENVIRONMENT_SET_VARIABLES):
		diatom_options_define_vars(data);
		return true;
	case MASK(RETRO_ENVIRONMENT_GET_VARIABLE): {
		struct retro_variable *v = data;
		if (!v) return false;
		v->value = diatom_options_get(v->key);
		/* Returning false means "no value set", which tells the core to keep
		 * its own default. That is the right answer for an unknown key. */
		return v->value != NULL;
	}
	case MASK(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE):
		/* Every frame, all six cores. Must stay this cheap. */
		*(bool *)data = diatom_options_take_update();
		return true;

	case MASK(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS):
		/* "B = Jump" for this game, offered free on every load. Kept rather
		 * than discarded because it is what makes a remap screen readable:
		 * without it a launcher can only offer `B -> ?`. ADR-0020. */
		capture_descriptors((const struct retro_input_descriptor *)data);
		return true;

	/* ---- accepted and ignored ------------------------------------------- */
	case MASK(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL):
	case MASK(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO):
		return true;

	/* ---- kept for achievements (ADR-0025) -------------------------------- */
	/* Both were in the ignore list above until 2026-08-29. The map in
	 * particular had to be captured rather than noted: the header says the
	 * frontend must keep its own copy, because the core's is valid only for
	 * the duration of this call. */
	case MASK(RETRO_ENVIRONMENT_SET_MEMORY_MAPS):
		diatom_cheevos_note_map(data);
		return true;
	case MASK(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS):
		diatom_cheevos_note_support(data ? *(const bool *)data : true);
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

/* Defined down with the map, which is the state it exists to advance. */
static void turbo_tick(void);

static void cb_input_poll(void)
{
	diatom_port_input_poll();
	turbo_tick();
}

/* Canonical button names. The protocol speaks these, never retropad numbers,
 * so libretro's numbering stops at this file exactly as it stops at the port
 * (ADR-0007, ADR-0020). Order matches the enum. */
static const char *const button_names[DIATOM_BTN_COUNT] = {
	"up", "down", "left", "right", "a", "b", "x", "y",
	"l1", "r1", "l2", "r2", "select", "start", "menu"
};

/* Canonical Diatom buttons -> retropad. Near-identity by design; its purpose is
 * keeping libretro.h out of the port, not translation.
 *
 * No longer const: ADR-0019 makes this the ONE layer a remap touches, so there
 * is one implementation of remapping however many ports exist. It is data, and
 * `identity_map` below is what RUN resets it to. */
static int button_map[DIATOM_BTN_COUNT] = {
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

static int identity_map[DIATOM_BTN_COUNT];
static bool g_map_saved;

/* Turbo, ADR-0028. A pulse is a property of a BINDING, so this table is written
 * and reset in lockstep with button_map above and never on its own: two tables
 * that both answer "what does X do" would be free to disagree, and preventing
 * that class of disagreement is what ADR-0020 is for.
 *
 * turbo_period[b] is half a cycle in FRAMES - `x:a~3` is three frames pressed,
 * three released - and 0 means an ordinary binding. Frames rather than
 * milliseconds because the core advances in frames, so the pulse is
 * deterministic, reproducible, and does not drift when the frame rate does.
 *
 * turbo_anchor[b] is the frame the source last went down. The phase is measured
 * from there rather than from an absolute counter so the first frame of a press
 * is always ON; anchored absolutely, half of all presses would open on an OFF
 * half-cycle and the button would feel like it had missed. */
#define TURBO_PERIOD_MAX 30       /* frames; a slower pulse is a typo */
static int  turbo_period[DIATOM_BTN_COUNT];
static long turbo_anchor[DIATOM_BTN_COUNT];
static uint32_t turbo_prev;      /* held mask at the previous poll, for edges */

/* Advanced once per frame by cb_input_poll, which libretro guarantees is called
 * once per retro_run before any input_state query. That makes it exactly the
 * right clock for pulsing input, and means no frame number has to be plumbed in
 * from the host. */
static long g_input_frame;

/* Labels the core gave us, indexed by canonical button after resolving the
 * active map - so they track a remap for free and a launcher never has to. */
static char g_label[DIATOM_BTN_COUNT][64];
static char g_retro_label[16][64];   /* by retropad id, as the core sends them */

static void relabel(void)
{
	int b;

	for (b = 0; b < DIATOM_BTN_COUNT; b++) {
		int id = button_map[b];
		g_label[b][0] = '\0';
		if (id >= 0 && id < (int)(sizeof g_retro_label / sizeof g_retro_label[0]))
			snprintf(g_label[b], sizeof g_label[b], "%s", g_retro_label[id]);
	}
}

static void capture_descriptors(const struct retro_input_descriptor *d)
{
	memset(g_retro_label, 0, sizeof g_retro_label);
	/* Port 0 only. Multiple controller ports are not in scope (ADR-0003 keeps
	 * input digital; nothing here has asked for a second pad yet), and taking
	 * port 0's labels is right for the single-player case that does exist. */
	for (; d && d->description; d++)
		if (d->port == 0 && d->device == RETRO_DEVICE_JOYPAD &&
		    d->id < sizeof g_retro_label / sizeof g_retro_label[0])
			snprintf(g_retro_label[d->id], sizeof g_retro_label[d->id],
			         "%s", d->description);
	relabel();
}

static int button_by_name(const char *name)
{
	int b;

	for (b = 0; b < DIATOM_BTN_COUNT; b++)
		if (!strcmp(button_names[b], name)) return b;
	return -1;
}

static void ensure_identity(void)
{
	if (g_map_saved) return;                 /* what we shipped with IS identity */
	memcpy(identity_map, button_map, sizeof identity_map);
	g_map_saved = true;
}

/* One frame of pulse bookkeeping, from the one callback libretro promises is
 * called exactly once per retro_run and before any input_state query. Doing it
 * here rather than inside cb_input_state is the whole reason two buttons cannot
 * disagree about where in the cycle they are: that function runs several times
 * a frame, once per queried id. ADR-0028. */
static void turbo_tick(void)
{
	uint32_t held;
	int b;

	g_input_frame++;
	held = diatom_port_input_state() & ~g_suppress;
	for (b = 0; b < DIATOM_BTN_COUNT; b++)
		if (turbo_period[b] &&
		    (held & DIATOM_BIT(b)) && !(turbo_prev & DIATOM_BIT(b)))
			turbo_anchor[b] = g_input_frame;
	turbo_prev = held;
}

/* RUN resets the map. Diatom is resident, so without this a table sent for one
 * game silently governs the next - a footgun that fires only on the games the
 * user did NOT configure, which is the worst possible place for it. ADR-0020. */
void diatom_input_reset_map(void)
{
	ensure_identity();
	memcpy(button_map, identity_map, sizeof button_map);
	memset(turbo_period, 0, sizeof turbo_period);
	memset(turbo_anchor, 0, sizeof turbo_anchor);
	turbo_prev = 0;
	relabel();
}

/* `spec` is ADR-0020's whole-table form: `a:b,x:none`, or `identity`.
 * Applied whole or not at all - a half-applied map is unplayable in a way that
 * is hard to diagnose, so a single bad pair rejects the message. */
bool diatom_input_set_map(const char *spec)
{
	int next[DIATOM_BTN_COUNT], next_period[DIATOM_BTN_COUNT];
	char buf[512], *save = NULL, *pair;

	ensure_identity();
	if (!spec || !*spec || !strcmp(spec, "identity")) {
		diatom_input_reset_map();
		return true;
	}

	/* Built beside the live table, never in it. Rejecting a bad pair after
	 * having already cleared the old map would be a silent third outcome:
	 * neither the requested map nor the one the launcher still believes in. */
	memcpy(next, identity_map, sizeof next);
	memset(next_period, 0, sizeof next_period);
	snprintf(buf, sizeof buf, "%s", spec);

	for (pair = strtok_r(buf, ",", &save); pair; pair = strtok_r(NULL, ",", &save)) {
		char *colon = strchr(pair, ':'), *tilde;
		int from, to, period = 0;

		if (!colon) return false;
		*colon = '\0';
		from = button_by_name(pair);
		/* MENU is unmappable BY CONSTRUCTION, not by policy - ADR-0019's
		 * fourth rule. Refusing it on either side is the only place that rule
		 * can actually be enforced, and letting it through would let a user
		 * map away the button that opens the screen which would undo it. */
		if (from < 0 || from == DIATOM_BTN_MENU) return false;

		/* ADR-0028's pulse. Split the target from its period BEFORE naming the
		 * target, so `a~3` reads as the button `a` at period 3 rather than as a
		 * button nobody has - which is also exactly why a Diatom older than
		 * this rejects the whole map instead of misreading it. */
		tilde = strchr(colon + 1, '~');
		if (tilde) {
			char *end;
			long v;

			*tilde = '\0';
			v = strtol(tilde + 1, &end, 10);
			/* `~0` is a plain binding in a costume, and the ceiling keeps a
			 * typo from producing a pulse slower than any game can use. */
			if (end == tilde + 1 || *end || v < 1 || v > TURBO_PERIOD_MAX)
				return false;
			period = (int)v;
		}

		if (!strcmp(colon + 1, "none")) {
			/* A period against an unbound button would store a pulse nothing
			 * can emit and read back as a map the launcher never sent. */
			if (period) return false;
			next[from] = -1;
			continue;
		}
		to = button_by_name(colon + 1);
		if (to < 0 || to == DIATOM_BTN_MENU) return false;
		next[from] = identity_map[to];
		next_period[from] = period;
	}

	memcpy(button_map, next, sizeof button_map);
	memcpy(turbo_period, next_period, sizeof turbo_period);
	/* Say what landed. A map arrives once a launch, from a config file nobody
	 * looks at, and silence here cost an hour on 2026-08-31: a turbo that did
	 * nothing was indistinguishable from a map that never arrived, from one
	 * that was refused, and from a test rig that could not press the button.
	 * Options already log this way, for the same reason. */
	{
		char msg[560];
		snprintf(msg, sizeof msg, "map: %s", spec);
		diatom_port_log(DIATOM_LOG_INFO, msg);
	}
	/* Any pulse in flight was measured from a press of a binding that no longer
	 * exists. Clearing turbo_prev as well guarantees the next poll sees a held
	 * button as a fresh edge and re-anchors it. */
	memset(turbo_anchor, 0, sizeof turbo_anchor);
	turbo_prev = 0;
	relabel();
	return true;
}

void diatom_input_emit_map(void)
{
	char out[512];
	size_t n = 0;
	int b;

	/* Not optional. `identity_map` is zero until something saves it, and every
	 * canonical button then compares unequal to its own identity and reports
	 * `none` - so an idle Diatom, asked for its map before any game had run,
	 * answered that every button was unbound. Found on hardware 2026-08-26;
	 * the desktop test missed it because RUN saves identity on the way past. */
	ensure_identity();
	out[0] = '\0';
	for (b = 0; b < DIATOM_BTN_COUNT; b++) {
		const char *to = "none";
		int t;

		if (button_map[b] == identity_map[b] && !turbo_period[b]) continue;
		for (t = 0; t < DIATOM_BTN_COUNT; t++)
			if (button_map[b] >= 0 && identity_map[t] == button_map[b]) {
				to = button_names[t];
				break;
			}
		n += (size_t)snprintf(out + n, sizeof out - n, "%s%s:%s",
		                      n ? "," : "", button_names[b], to);
		/* Checked between the two writes, not only after: snprintf returns what
		 * it WOULD have written, so a second call with an already-overflowed n
		 * would underflow `sizeof out - n` and write past the buffer. */
		if (n >= sizeof out) break;
		if (turbo_period[b])
			n += (size_t)snprintf(out + n, sizeof out - n, "~%d",
			                      turbo_period[b]);
		if (n >= sizeof out) break;
	}
	diatom_proto_send("MAP\tmap=%s", out[0] ? out : "identity");
}

void diatom_input_emit_labels(void)
{
	int b, n = 0;

	for (b = 0; b < DIATOM_BTN_COUNT; b++)
		if (g_label[b][0]) n++;

	/* A button with no label is omitted and does not count. Many cores
	 * describe nothing at all, several describe only some buttons, and a
	 * button mapped to `none` has nothing to describe - one rule covers all
	 * three, and a launcher shows its own name for whatever is absent. */
	diatom_proto_send("INPUTS\tcount=%d", n);
	for (b = 0; b < DIATOM_BTN_COUNT; b++)
		if (g_label[b][0])
			diatom_proto_send("INPUT\tid=%s\tlabel=%s",
			                  button_names[b], g_label[b]);
}

static int16_t cb_input_state(unsigned port, unsigned device,
                              unsigned index, unsigned id)
{
	uint32_t state;
	int b;

	(void)index;
	if (port != 0 || device != RETRO_DEVICE_JOYPAD) return 0;

	state = diatom_port_input_state() & ~g_suppress;

	/* OR, not first-match. Under an identity map no two canonical buttons
	 * share a target so returning the first was always correct; remapping
	 * makes sharing legal, and `SETMAP map=x:b,y:b` must fire for either.
	 * Returning early would have silently dropped one of them.
	 *
	 * A pulse is applied PER SOURCE, inside this loop, before the OR has
	 * finished: holding A while a turbo-X also targets A must give a
	 * continuously held A rather than an interference pattern between the two.
	 * So a source in its OFF half-cycle keeps looking instead of returning 0.
	 * ADR-0028. */
	for (b = 0; b < DIATOM_BTN_COUNT; b++) {
		if (button_map[b] != (int)id || !(state & DIATOM_BIT(b))) continue;
		if (!turbo_period[b]) return 1;
		if (((g_input_frame - turbo_anchor[b]) / turbo_period[b]) % 2 == 0)
			return 1;
	}
	return 0;
}

/* Video and audio callbacks live in main.c, which owns the frame buffer and
 * the pacing decision - ADR-0007 has the frame consumed after retro_run
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

	/* Bind the option table BEFORE set_environment: a core declares its
	 * options from inside that call, and they must land in its own table. */
	diatom_options_bind(c);

	c->set_environment(env_cb);
	c->set_video_refresh(cb_video);
	c->set_audio_sample(cb_audio_sample);
	c->set_audio_sample_batch(cb_audio_batch);
	c->set_input_poll(cb_input_poll);
	c->set_input_state(cb_input_state);
}
