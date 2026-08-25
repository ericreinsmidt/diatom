/* Diatom - a minimal libretro frontend.
 *
 * Standalone is the primary mode:  diatom --core X.so --rom game.nes
 * A host application driving a resident Diatom over a socket (ADR-0009) is an
 * additional mode, not the fundamental one. Designing for a program that stands
 * alone is a stricter test than designing for one embedder.
 *
 * The frame is copied inside the core's video callback and presented AFTER
 * retro_run returns - ADR-0007. Cores emit video and audio from inside
 * retro_run in an order that varies, so presenting from the callback means
 * committing to a frame before knowing how much audio it produced.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <signal.h>

#include "diatom.h"

/* --- the frame the core last handed us ----------------------------------- */
static void       *g_frame;
static size_t      g_frame_cap;
static int         g_frame_w, g_frame_h;
static size_t      g_frame_pitch;
static bool        g_frame_fresh;

static diatom_core     *g_core;   /* resident, never unloaded - ADR-0006 */
static diatom_policy    g_policy;
static diatom_port_caps g_caps;
static diatom_rect      g_dst;   /* locked at load - ADR-0011 */

/* Set from a signal handler and read by the frame loop. The handler does NO
 * file I/O: it flips this, the loop notices after retro_run returns and does
 * the flush itself. Measured on the Brick, SIGTERM arrives about 810 ms before
 * the process dies on power-off, and one frame of retro_run is ~10 ms of that
 * budget - a 1% price for not calling malloc or open from a handler. */
static volatile sig_atomic_t g_terminate;
static bool g_quit_requested;   /* launcher said QUIT */

static void on_terminate(int sig) { (void)sig; g_terminate = 1; }

/* Display mode state. The BASE geometry is kept because that, not the current
 * geometry, is what a rect is ever computed from: ADR-0011's invariant is
 * "never recompute from a mid-run SET_GEOMETRY", which a deliberate user-driven
 * mode change does not violate as long as it recomputes from the base. */
static int           g_base_w, g_base_h;
static double        g_base_aspect;
static int           g_mode;
static diatom_filter g_filter;

/* Per-combination measurement, so a mode that looks best can be checked
 * against what it costs. Printed on every change and again at exit. */
#define SLOT(m, f) ((m) * 2 + (int)(f))
static struct {
	long     frames;
	uint64_t present_us;
	long     resyncs;
} g_stat[32];

/* Called from inside retro_run. Copy and return; do not present here. */
void diatom_on_video(const void *data, unsigned w, unsigned h, size_t pitch)
{
	size_t need;

	if (!data) return;          /* dupe: keep the previous frame */
	need = pitch * h;
	if (need > g_frame_cap) {
		void *p = realloc(g_frame, need);
		if (!p) return;
		g_frame = p;
		g_frame_cap = need;
	}
	memcpy(g_frame, data, need);
	g_frame_w = (int)w;
	g_frame_h = (int)h;
	g_frame_pitch = pitch;
	g_frame_fresh = true;
}

void diatom_on_audio_batch_store(const int16_t *data, size_t frames)
{
	diatom_audio_push(data, frames);
}

static void usage(void)
{
	int i;
	fprintf(stderr,
		"usage: diatom --core <core.so> --rom <file> [--system <dir>] [--save <dir>]\n"
		"              [--display <mode>] [--filter nearest|sharp]\n"
		"              [--load-state <file>] [--state-on-exit <file>]\n"
		"              [--frames <n>] [--shot <file.bmp>]\n"
		"              [--socket <path>]   launcher protocol, ADR-0009\n"
		"              [--core-option key=value] ...   repeatable\n"
		"              [--list-options]    what this core offers, then exit\n"
		"\nSRAM is automatic: read at load, written when it changes, flushed on\n"
		"exit and on SIGTERM. Save states take paths, never slot numbers - slots\n"
		"belong to the launcher (ADR-0016).\n"
		"\non device: SELECT+R1 / SELECT+L1 changes mode, SELECT+A toggles filter\n\n");
	for (i = 0; i < diatom_mode_count; i++)
		fprintf(stderr, "  %-10s %s\n",
		        diatom_modes[i].name, diatom_modes[i].note);
}

static const char *filter_name(diatom_filter f)
{
	return f == DIATOM_FILTER_SHARP ? "sharp" : "nearest";
}

static void apply_display(int mode, diatom_filter filter)
{
	g_mode   = mode;
	g_filter = filter;
	g_dst = diatom_scale_rect(diatom_modes[mode].mode, g_base_w, g_base_h,
	                          g_base_aspect, g_caps.surface_w, g_caps.surface_h);
	printf("diatom: display %-10s %-7s %4dx%-4d at %4d,%-4d  %s\n",
	       diatom_modes[mode].name, filter_name(filter),
	       g_dst.w, g_dst.h, g_dst.x, g_dst.y, diatom_modes[mode].note);
	fflush(stdout);
}

/* Report what a combination cost, so the look and the price are read together.
 * A mode that is prettier and misses frames is a trade, not a win. */
static void report_slot(int mode, diatom_filter filter)
{
	int  s = SLOT(mode, filter);
	long f = g_stat[s].frames;

	if (f <= 0) return;
	printf("diatom:   %-10s %-7s %5ld frames, present avg %5.2f ms, %ld resync(s)\n",
	       diatom_modes[mode].name, filter_name(filter), f,
	       (double)g_stat[s].present_us / (double)f / 1000.0,
	       g_stat[s].resyncs);
	fflush(stdout);
}

/* SELECT is the modifier: SELECT+R1 and SELECT+L1 step the mode, SELECT+A
 * toggles the filter, all edge-triggered. The keys involved are hidden from
 * the core while SELECT is held; the very first frame of the press still
 * leaks, because the core reads input inside retro_run before this runs.
 * Harmless here and not worth a pre-run poll to fix.
 *
 * Returns 1 if anything changed. */
static int display_chord(uint32_t buttons, uint32_t prev)
{
	static const uint32_t chord = DIATOM_BIT(DIATOM_BTN_SELECT);
	uint32_t pressed = buttons & ~prev;

	if (!(buttons & chord)) {
		diatom_env_suppress(0);
		return 0;
	}
	diatom_env_suppress(chord | DIATOM_BIT(DIATOM_BTN_L1)
	                          | DIATOM_BIT(DIATOM_BTN_R1)
	                          | DIATOM_BIT(DIATOM_BTN_A));

	if (pressed & DIATOM_BIT(DIATOM_BTN_R1)) {
		report_slot(g_mode, g_filter);
		apply_display((g_mode + 1) % diatom_mode_count, g_filter);
		return 1;
	}
	if (pressed & DIATOM_BIT(DIATOM_BTN_L1)) {
		report_slot(g_mode, g_filter);
		apply_display((g_mode + diatom_mode_count - 1) % diatom_mode_count,
		              g_filter);
		return 1;
	}
	if (pressed & DIATOM_BIT(DIATOM_BTN_A)) {
		report_slot(g_mode, g_filter);
		apply_display(g_mode, g_filter == DIATOM_FILTER_SHARP
		                    ? DIATOM_FILTER_NEAREST : DIATOM_FILTER_SHARP);
		return 1;
	}
	return 0;
}

/* One game, start to finish. Extracted so the protocol loop (ADR-0009) can run
 * it repeatedly in a process that never exits - which is what makes a warm
 * launch ~35 ms instead of ~700 ms, measured. Standalone mode calls it once.
 *
 * Returns 0 on a clean run, or a non-zero code that main turns into an exit
 * status or an ERROR message depending on how Diatom was started. */
typedef struct {
	const char   *core, *rom, *shot, *state_load, *state_exit;
	long          limit;
	int           mode;
	diatom_filter filter;
	bool          list_only;
} diatom_session;

static int run_session(const diatom_session *sn)
{
	struct retro_system_av_info av;
	struct retro_system_info si;
	double   frame_us, next_us;
	uint64_t t_start;
	uint32_t buttons = 0, prev_buttons = 0;
	long frames = 0, geom_changes = 0, resyncs = 0;
	size_t q_min = (size_t)-1, q_max = 0;
	bool stop = false;
	int i;

	/* Per-game state that must not carry over from the previous session. */
	memset(g_stat, 0, sizeof g_stat);
	g_frame_w = g_frame_h = 0;
	g_frame_pitch = 0;
	g_frame_fresh = false;

	g_core = diatom_core_resident(sn->core);
	if (!g_core) {
		diatom_proto_send("ERROR\tcode=core_missing\tmsg=%s", sn->core);
		return 3;
	}
	diatom_env_bind(g_core, &g_policy, &g_caps);

	g_core->get_system_info(&si);
	printf("diatom: %s %s\n",
	       si.library_name ? si.library_name : "?",
	       si.library_version ? si.library_version : "?");

	/* Most cores declare their options during core open, but not all: some
	 * wait until content is loaded, because what they offer depends on the
	 * ROM. So list after loading when a ROM was given, and before when it was
	 * not - listing what a core offers should not *require* owning a game. */
	if (sn->list_only && !sn->rom) {
		diatom_options_list();
		return 0;
	}

	if (!sn->rom && !g_policy.supports_no_game) {
		fprintf(stderr, "diatom: this core needs content; pass --rom\n");
		diatom_proto_send("ERROR\tcode=rom_unreadable\tmsg=core needs content");
		return 4;
	}
	if (!diatom_core_start(g_core, sn->rom)) {
		diatom_proto_send("ERROR\tcode=rom_unreadable\tmsg=core refused the rom");
		return 4;
	}

	if (sn->list_only) {
		diatom_options_list();
		diatom_core_stop(g_core);
		return 0;
	}

	/* Genesis 3-button vs 6-button is a correctness issue, not a preference -
	 * some early games misbehave with a 6-button pad attached, which is why the
	 * real pad has a Mode switch. Digital-only (ADR-0003) removes axes, not
	 * device types. */
	g_core->set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

	/* SRAM first: it is the game's own data and is not optional. A state, if
	 * the launcher asked for one, is layered on top - and a state that fails
	 * to load is not an error, it just means the game starts normally. That
	 * fallback is what makes resume-by-default safe across a core update. */
	diatom_save_init(g_core, g_policy.save_dir, sn->rom);
	if (sn->state_load && !diatom_state_load(g_core, sn->state_load))
		printf("diatom: no usable state at %s; starting the game normally\n",
		       sn->state_load);

	{
		struct sigaction sa;
		memset(&sa, 0, sizeof sa);
		sa.sa_handler = on_terminate;
		sigaction(SIGTERM, &sa, NULL);
		sigaction(SIGINT,  &sa, NULL);
		sigaction(SIGHUP,  &sa, NULL);
	}

	g_core->get_system_av_info(&av);
	printf("diatom: %ux%u (max %ux%u) aspect %.4f, %.4f fps, %.0f Hz -> %d Hz\n",
	       av.geometry.base_width, av.geometry.base_height,
	       av.geometry.max_width, av.geometry.max_height,
	       (double)av.geometry.aspect_ratio,
	       av.timing.fps, av.timing.sample_rate, g_caps.audio_rate);

	diatom_audio_configure(av.timing.sample_rate, g_caps.audio_rate,
	                       g_caps.audio_buffer_frames);
	diatom_audio_prime();

	/* The rect is computed from BASE geometry and does not move again unless
	 * the user asks - ADR-0011.
	 *
	 * Cores announce hires by calling SET_GEOMETRY with a larger base_width
	 * mid-run (measured: 3 of 6 do this). Recomputing an integer factor from
	 * the new width collapses 3x to 1x and the picture shrinks to a fifth.
	 * Holding the rect fixed keeps the picture the same size AND is more
	 * faithful: SNES and PC Engine hires pixels are physically half-width, so
	 * 512 columns belong in the same screen width as 256.
	 *
	 * A mode change recomputes from these same base values, never from the
	 * current geometry, which is what keeps the invariant intact. */
	g_base_w      = (int)av.geometry.base_width;
	g_base_h      = (int)av.geometry.base_height;
	g_base_aspect = (double)av.geometry.aspect_ratio;
	apply_display(sn->mode, sn->filter);

	/* Pace against a monotonic clock at the core's own rate, on an ABSOLUTE
	 * schedule kept in floating point.
	 *
	 * Absolute matters: an incremental "sleep frame_us each time" accumulates
	 * every scheduler overshoot forever, while a running deadline absorbs them -
	 * a long sleep is followed by a correspondingly short one.
	 *
	 * Floating point matters too, if less: 1000000/59.7275 is 16742.63us, and
	 * truncating loses 38ms per hour.
	 *
	 * Audio drift is not this loop's problem. It is handled by rate control,
	 * because no panel and no core will ever agree on a rate. */
	frame_us = 1000000.0 / (av.timing.fps > 0 ? av.timing.fps : 60.0);

	/* The display handover, and the reason ADR-0009 separates ERROR from EXIT:
	 * from here the launcher must stop drawing. Announced before the warmup,
	 * because the warmup already puts frames on the panel. */
	diatom_proto_send("RUNNING");

	/* Warm up before starting the clock. The first frames create the texture,
	 * fault in code paths and prime the audio device; measured on a COLD
	 * process, they overrun the frame budget badly enough to trip a resync
	 * every single run. Timing them reports a rate the loop never actually
	 * sustains - and, worse, hides whether the steady-state loop is correct.
	 *
	 * Note for anyone tempted to skip this when resident: measured 2026-08-25
	 * across five launches in one process, the warmup costs 23-34 ms every
	 * time, flat. It is not cold-start overhead that residency has already
	 * paid; it is three real frames of emulation and blitting at ~8 ms each.
	 * Skipping it would save frames, not overhead. */
	{
		uint64_t w0 = diatom_port_now_us();
		int w;
		for (w = 0; w < 3; w++) {
			g_core->run();
			diatom_port_present(g_frame, g_frame_w, g_frame_h, g_frame_pitch,
			                    g_policy.pixfmt, g_dst,
			                    g_filter);
		}
		printf("diatom: warmup 3 frames in %.1f ms\n",
		       (diatom_port_now_us() - w0) / 1000.0);
	}

	t_start = diatom_port_now_us();
	next_us = (double)t_start;

	while (!diatom_port_should_quit() && !g_terminate && !stop &&
	       (sn->limit <= 0 || frames < sn->limit)) {
		uint64_t now;

		g_frame_fresh = false;
		g_core->run();
		frames++;

		/* Noted, not acted on: the rect is locked (ADR-0011). The port scales
		 * whatever arrives into it, so a hires frame keeps its screen size. */
		if (diatom_env_geometry_changed()) geom_changes++;

		{
			uint64_t p0 = diatom_port_now_us();
			diatom_port_present(g_frame_fresh ? g_frame : NULL,
			                    g_frame_w, g_frame_h, g_frame_pitch,
			                    g_policy.pixfmt, g_dst,
			                    g_filter);
			g_stat[SLOT(g_mode, g_filter)].present_us
				+= diatom_port_now_us() - p0;
			g_stat[SLOT(g_mode, g_filter)].frames++;
		}

		/* Display switching lives after present, so the timing above covers
		 * exactly one combination's work. */
		diatom_save_tick();

		if (diatom_proto_active()) {
			diatom_msg m;
			switch (diatom_proto_poll(&m, 0, true)) {
			case DIATOM_MSG_STOP: stop = true; break;
			case DIATOM_MSG_QUIT: stop = true; g_quit_requested = true; break;
			/* HANGUP is deliberately not a stop. ADR-0008 exists so a
			 * launcher that died cannot take a running game with it. */
			default: break;
			}
		}

		buttons = diatom_port_input_state();
		display_chord(buttons, prev_buttons);
		prev_buttons = buttons;

		{
			size_t q = diatom_port_audio_queued();
			if (q < q_min) q_min = q;
			if (q > q_max) q_max = q;
		}
		diatom_audio_sync();

		next_us += frame_us;
		now = diatom_port_now_us();

		if (next_us > (double)now) {
			struct timespec ts;
			double d = next_us - (double)now;
			ts.tv_sec  = (time_t)(d / 1000000.0);
			ts.tv_nsec = (long)(fmod(d, 1000000.0) * 1000.0);
			nanosleep(&ts, NULL);
		} else if ((double)now - next_us > frame_us * 4.0) {
			/* More than four frames behind. Something stalled - the scheduler,
			 * a page fault, a slow core - and trying to catch up would just run
			 * fast for a while, which looks worse than dropping the debt.
			 * Counted, because a loop that resyncs often is a loop that is
			 * lying about its frame rate. */
			next_us = (double)now;
			resyncs++;
			g_stat[SLOT(g_mode, g_filter)].resyncs++;
		}
		/* Otherwise keep the debt and let the next short sleep repay it. */
	}

	/* Read the clock before the capture: converting and writing a full-screen
	 * BMP costs hundreds of milliseconds, and it happens after the last frame.
	 * Measured on the Brick: leaving it inside the timed span understated a
	 * perfectly paced 59.73fps loop as 58.1 - a measurement bug wearing the
	 * costume of a pacing bug. */
	if (g_terminate)
		printf("diatom: terminated by signal; saving\n");

	/* Both paths, and in this order: the game's own save first, because losing
	 * it is a defect, then the state, which is a convenience. */
	diatom_save_shutdown();
	if (sn->state_exit) diatom_state_save(g_core, sn->state_exit);

	{
		uint64_t t_end = diatom_port_now_us();

		if (sn->shot)
			printf("diatom: capture %s: %s\n", sn->shot,
			       diatom_port_capture(sn->shot) ? "ok" : "FAILED");

		double secs = (t_end - t_start) / 1000000.0;
		printf("diatom: %ld frames in %.2fs = %.2f fps (target %.4f)\n",
		       frames, secs, secs > 0 ? frames / secs : 0.0, av.timing.fps);
		printf("diatom: %ld geometry change(s), last rect %dx%d at %d,%d\n",
		       geom_changes, g_dst.w, g_dst.h, g_dst.x, g_dst.y);
		printf("diatom: %ld resync(s)\n", resyncs);
		printf("diatom: per display mode:\n");
		for (i = 0; i < diatom_mode_count; i++) {
			report_slot(i, DIATOM_FILTER_NEAREST);
			report_slot(i, DIATOM_FILTER_SHARP);
		}
		printf("diatom: audio queued min %zu max %zu final %zu, target %d, capacity %d\n",
		       q_min == (size_t)-1 ? 0 : q_min, q_max,
		       diatom_port_audio_queued(),
		       g_caps.audio_buffer_frames / 2, g_caps.audio_buffer_frames);
		printf("diatom: rate control drift %+.3f%% (max %+.3f%%)\n",
		       diatom_audio_ratio_drift() * 100.0, 0.5);
	}

	diatom_core_stop(g_core);
	/* No dlclose. Ever. ADR-0006. */

	/* The game ran and stopped, which is EXIT rather than ERROR whatever the
	 * reason. The launcher may take the display back now. */
	diatom_proto_send("EXIT\treason=%s", g_terminate ? "user" : "user");
	return 0;
}

int main(int argc, char **argv)
{
	const char *core_path = NULL, *rom_path = NULL, *shot_path = NULL;
	/* ADR-0014: stretch by default, judged on the panel. A handheld's screen
	 * is its whole interface, and full use of it beat both the letterbox and
	 * the crop. nearest because sharp earned nothing visible at these factors
	 * and is the only thing that has made this loop miss a frame. */
	const char *display = "stretch", *filter = "nearest";
	const char *state_load = NULL, *state_exit = NULL;
	const char *sock = getenv("DIATOM_SOCKET");
	bool list_options = false;
	diatom_filter start_filter;
	long limit = 0;
	int i, start_mode = -1;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--core") && i + 1 < argc) core_path = argv[++i];
		else if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom_path = argv[++i];
		else if (!strcmp(argv[i], "--system") && i + 1 < argc) g_policy.system_dir = argv[++i];
		else if (!strcmp(argv[i], "--save") && i + 1 < argc) g_policy.save_dir = argv[++i];
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) limit = strtol(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot_path = argv[++i];
		else if (!strcmp(argv[i], "--display") && i + 1 < argc) display = argv[++i];
		else if (!strcmp(argv[i], "--filter") && i + 1 < argc) filter = argv[++i];
		else if (!strcmp(argv[i], "--load-state") && i + 1 < argc) state_load = argv[++i];
		else if (!strcmp(argv[i], "--state-on-exit") && i + 1 < argc) state_exit = argv[++i];
		else if (!strcmp(argv[i], "--socket") && i + 1 < argc) sock = argv[++i];
		else if (!strcmp(argv[i], "--list-options")) list_options = true;
		else if (!strcmp(argv[i], "--core-option") && i + 1 < argc) {
			/* Recorded now, applied when the core declares its options - the
			 * command line is parsed long before retro_set_environment runs. */
			char *kv = argv[++i], *eq = strchr(kv, '=');
			if (!eq) { fprintf(stderr, "diatom: --core-option wants key=value\n"); return 1; }
			*eq = '\0';
			diatom_options_set(kv, eq + 1);
			*eq = '=';
		}
		else { usage(); return 1; }
	}
	/* A core is required standalone, but under the protocol it arrives with
	 * each RUN, so its absence is not an error at startup. */
	if (!core_path && !sock) { usage(); return 1; }

	for (i = 0; i < diatom_mode_count; i++)
		if (!strcmp(display, diatom_modes[i].name)) start_mode = i;
	if (start_mode < 0) {
		fprintf(stderr, "diatom: unknown display mode '%s'\n", display);
		usage();
		return 1;
	}
	if (!strcmp(filter, "sharp"))        start_filter = DIATOM_FILTER_SHARP;
	else if (!strcmp(filter, "nearest")) start_filter = DIATOM_FILTER_NEAREST;
	else {
		fprintf(stderr, "diatom: unknown filter '%s'\n", filter);
		usage();
		return 1;
	}

	if (!g_policy.system_dir) g_policy.system_dir = ".";
	if (!g_policy.save_dir)   g_policy.save_dir   = ".";
	g_policy.pixfmt = DIATOM_PIX_RGB565;   /* libretro's default is 0RGB1555,
	                                          which Diatom refuses; every core
	                                          measured picked RGB565 anyway */

	if (!diatom_port_init(&g_caps)) {
		fprintf(stderr, "diatom: port init failed\n");
		return 2;
	}

	/* Two modes, and standalone is the primary one - designing for a program
	 * that stands alone is a stricter test than designing for one embedder.
	 *
	 * The socket mode is what makes launches fast: the process outlives the
	 * game, so SDL init and every core dlopen are paid once at boot rather
	 * than per launch. Measured: ~35 ms warm against 625-750 ms cold. */
	if (sock) {
		if (!diatom_proto_listen(sock)) return 5;

		while (!g_quit_requested && !g_terminate) {
			diatom_msg m;
			diatom_session sn;

			/* Idle: block. Nothing is on screen, so there is nothing to
			 * pace and no reason to spin. */
			if (diatom_proto_poll(&m, -1, false) != DIATOM_MSG_RUN) continue;
			if (!m.core[0]) {
				diatom_proto_send("ERROR\tcode=core_missing\tmsg=no core given");
				continue;
			}

			memset(&sn, 0, sizeof sn);
			sn.core   = m.core;
			sn.rom    = m.rom[0] ? m.rom : NULL;
			sn.mode   = start_mode;
			sn.filter = start_filter;
			run_session(&sn);   /* its own ERROR/EXIT is the report */
		}
		diatom_proto_close();
	} else {
		diatom_session sn = { core_path, rom_path, shot_path,
		                      state_load, state_exit,
		                      limit, start_mode, start_filter,
		                      list_options };
		int rc = run_session(&sn);
		if (rc) return rc;
	}
	diatom_port_shutdown();
	free(g_frame);
	return 0;
}
