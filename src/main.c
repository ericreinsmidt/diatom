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
#include <unistd.h>

#include "diatom.h"

/* --- the frame the core last handed us ----------------------------------- */
static void       *g_frame;
static size_t      g_frame_cap;
static int         g_frame_w, g_frame_h;
static size_t      g_frame_pitch;
static bool        g_frame_fresh;

/* ADR-0020's state plane, defined below but reachable from the menu loop that
 * comes first. */
/* The session, up here because menu_pause reads its persistence paths. The
 * lifecycle comment lives with run_session_inner below, which is what actually
 * runs one. */
typedef struct diatom_session {
	const char   *core, *rom, *shot, *state_load, *state_exit;
	const char   *preview;    /* BMP of the frame, written on pause and exit */
	const char   *firmware;   /* what the launcher says this content needs */
	long          limit;
	int           mode;
	diatom_filter filter;
	bool          list_only;
} diatom_session;

static bool state_plane_msg(const diatom_msg *m);
static void levels_forget(void);
static void apply_display(int mode, diatom_filter filter);

static diatom_core     *g_core;   /* resident, never unloaded - ADR-0006 */
static diatom_policy    g_policy;

/* The preview is the CORE'S frame, not the screen. Three reasons, none of
 * them taste: it is 8-30x smaller (a 256x224 frame against a 1024x768 panel),
 * it never contains the OSD bar or a menu, and it needs nothing from the port
 * - the host already holds the last frame for dupe handling. minarch's
 * previews are small for the same reason a launcher cares: it decodes one per
 * visible card, on a shelf of dozens. BMP because that is what PlayOS already
 * reads, top-down rows because that is what BMP wants for a positive height...
 * negative height, rather - top-down needs biHeight < 0, and getting that
 * wrong renders every card upside down. */
static bool write_preview(const char *path)
{
	FILE *f;
	unsigned char hdr[54];
	int w = g_frame_w, h = g_frame_h, y, x;
	size_t row = (size_t)w * 3, pad = (4 - (row & 3)) & 3;
	unsigned int size = 54 + (unsigned int)((row + pad) * (size_t)h);

	if (!g_frame || w <= 0 || h <= 0) return false;
	f = fopen(path, "wb");
	if (!f) return false;

	memset(hdr, 0, sizeof hdr);
	hdr[0] = 'B'; hdr[1] = 'M';
	hdr[2]  = (unsigned char)size;       hdr[3]  = (unsigned char)(size >> 8);
	hdr[4]  = (unsigned char)(size >> 16); hdr[5] = (unsigned char)(size >> 24);
	hdr[10] = 54;
	hdr[14] = 40;
	hdr[18] = (unsigned char)w; hdr[19] = (unsigned char)(w >> 8);
	hdr[22] = (unsigned char)h; hdr[23] = (unsigned char)(h >> 8);
	hdr[26] = 1;                          /* planes */
	hdr[28] = 24;                         /* bpp */
	fwrite(hdr, 1, sizeof hdr, f);

	/* Bottom-up, as positive-height BMP requires. */
	for (y = h - 1; y >= 0; y--) {
		const unsigned char *src = (const unsigned char *)g_frame
		                         + (size_t)y * g_frame_pitch;
		unsigned char px[3] = { 0, 0, 0 };

		for (x = 0; x < w; x++) {
			if (g_policy.pixfmt == DIATOM_PIX_RGB565) {
				unsigned v = ((const unsigned short *)src)[x];
				px[2] = (unsigned char)(((v >> 11) & 0x1f) * 255 / 31);
				px[1] = (unsigned char)(((v >>  5) & 0x3f) * 255 / 63);
				px[0] = (unsigned char)(( v        & 0x1f) * 255 / 31);
			} else {   /* XRGB8888 */
				unsigned v = ((const unsigned *)src)[x];
				px[2] = (unsigned char)(v >> 16);
				px[1] = (unsigned char)(v >> 8);
				px[0] = (unsigned char)v;
			}
			fwrite(px, 1, 3, f);
		}
		if (pad) { unsigned char z[3] = {0,0,0}; fwrite(z, 1, pad, f); }
	}
	fclose(f);
	return true;
}

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

/* --- crash reporting - ADR-0009's `EXIT reason=crash` --------------------- */

/* A core is dlopen'd into this address space (ADR-0006), so a core that dies
 * kills Diatom. That rules out the trick on_terminate uses: there is no "after
 * retro_run returns" to defer the work to, because the process is going away
 * inside the handler.
 *
 * Which side of RUNNING the crash lands on decides the message, and that comes
 * straight from proto.c's rule rather than from taste. A core that dies while
 * loading means the game never started, so the launcher is still drawing and
 * must be told ERROR. A core that dies mid-game means it ran and stopped, so
 * the launcher is NOT drawing and must be told EXIT to take the display back.
 * Sending the wrong one leaves a handheld showing a dead frame.
 *
 * What this deliberately does not do is try to survive. Continuing after a
 * fault in a dlopen'd core - longjmp back to the protocol loop, accept the next
 * RUN - is tempting precisely because residency makes the process valuable, and
 * it is wrong: the address space is already suspect, the core's own state is
 * arbitrary, and the next game would inherit both. */
enum { PHASE_IDLE, PHASE_LOADING, PHASE_RUNNING };
static volatile sig_atomic_t g_phase;

/* Complete lines, one per signal per phase, because a handler cannot build a
 * string: snprintf is not async-signal-safe. Repetitive on purpose - the
 * alternative is assembling the line in pieces, and a torn write would put a
 * malformed message on the socket at the worst possible moment.
 *
 * `signal=` is an extra key, which ADR-0009 makes free: unknown keys are
 * ignored, so an older launcher sees a plain crash and a newer one can log
 * which signal it was. That name is the only trace that survives - the process
 * is about to die, and nothing else records it. */
static const struct {
	int         sig;
	const char *running;   /* crashed after RUNNING: it ran and stopped */
	const char *loading;   /* crashed before RUNNING: it never started */
} crash_lines[] = {
	{ SIGSEGV, "EXIT\treason=crash\tsignal=SIGSEGV\n", "ERROR\tcode=crash\tmsg=SIGSEGV\n" },
	{ SIGBUS,  "EXIT\treason=crash\tsignal=SIGBUS\n",  "ERROR\tcode=crash\tmsg=SIGBUS\n"  },
	{ SIGILL,  "EXIT\treason=crash\tsignal=SIGILL\n",  "ERROR\tcode=crash\tmsg=SIGILL\n"  },
	{ SIGFPE,  "EXIT\treason=crash\tsignal=SIGFPE\n",  "ERROR\tcode=crash\tmsg=SIGFPE\n"  },
	{ SIGABRT, "EXIT\treason=crash\tsignal=SIGABRT\n", "ERROR\tcode=crash\tmsg=SIGABRT\n" },
};

/* Deliberately no diatom_port_present_stop() below. It waits on a condition
 * variable, which is not async-signal-safe, and a crashed game that tears on
 * its way out is a far better outcome than one that deadlocks in a signal
 * handler with the display held. The tear is milliseconds; the deadlock would
 * be a power cycle. */
static void on_crash(int sig)
{
	size_t i;

	/* PHASE_IDLE says no game was in flight, so there is nothing to report
	 * about one. Diatom dying between sessions reaches the launcher as the
	 * socket closing, which ADR-0009 already relies on for liveness. */
	if (g_phase != PHASE_IDLE)
		for (i = 0; i < sizeof crash_lines / sizeof crash_lines[0]; i++)
			if (crash_lines[i].sig == sig) {
				diatom_proto_emit_fatal(g_phase == PHASE_RUNNING
				                        ? crash_lines[i].running
				                        : crash_lines[i].loading);
				break;
			}

	/* Then die of the original signal with its original disposition. Anything
	 * else lies about how the process ended: the wait status is how a
	 * supervisor tells a crash from a clean stop, and the core dump is how
	 * anyone finds out why. Returning instead would re-run the faulting
	 * instruction and loop here forever.
	 *
	 * signal() and raise() are both on the async-signal-safe list; sigaction()
	 * with a memset struct is not, which is why the older-looking call is the
	 * correct one here. */
	signal(sig, SIG_DFL);
	raise(sig);
}

/* A core that calls exit() on a fatal error - some do - raises no signal, so
 * the handler above never runs and the launcher would wait forever on a
 * process that has already gone. Not signal context, so the ordinary send is
 * fine here.
 *
 * Fires on every exit including the clean ones, which is what PHASE_IDLE is
 * for: after a normal EXIT there is nothing left to say. */
static void on_exit_hook(void)
{
	if (g_phase == PHASE_IDLE) return;
	if (g_phase == PHASE_RUNNING)
		diatom_proto_send("EXIT\treason=crash\tmsg=core called exit");
	else
		diatom_proto_send("ERROR\tcode=crash\tmsg=core called exit");
}

/* 64 KB, a fixed size rather than SIGSTKSZ: since glibc 2.34 that expands to a
 * sysconf() call, which cannot size an array at file scope, and the Brick
 * builds against a newer glibc than the desktop does. */
static char g_crash_stack[65536];

static void install_crash_handlers(void)
{
	static stack_t ss;
	struct sigaction sa;
	size_t i;

	/* A private stack, because the failure most likely to arrive here is a
	 * core recursing without bound, and a SIGSEGV raised by an exhausted stack
	 * cannot run a handler on that same stack.
	 *
	 * MEASURED on the Brick, 2026-08-25, A/B on one binary an hour apart -
	 * stubcore recursing until the 8 MB stack is gone:
	 *
	 *   SA_ONSTACK      EXIT reason=crash signal=SIGSEGV
	 *   without it      nothing; the launcher saw only the socket close
	 *
	 * So this is load-bearing rather than defensive. Both runs still died on
	 * signal 11 - what the flag buys is the report, not the death.
	 *
	 * Per-thread, so this covers the main thread only. The port's flip thread
	 * does not recurse; a fault there still reports, just not the overflow. */
	ss.ss_sp    = g_crash_stack;
	ss.ss_size  = sizeof g_crash_stack;
	ss.ss_flags = 0;
	sigaltstack(&ss, NULL);

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_crash;
	sa.sa_flags   = SA_ONSTACK;
	/* Block everything for the duration: a second fault inside the handler
	 * then cannot re-enter it, and the kernel kills us outright instead. That
	 * is the right outcome - it is already the second failure. */
	sigfillset(&sa.sa_mask);
	for (i = 0; i < sizeof crash_lines / sizeof crash_lines[0]; i++)
		sigaction(crash_lines[i].sig, &sa, NULL);

	atexit(on_exit_hook);
}

/* How long a rect has to survive before it counts as the real mode. Herzog
 * Zwei, the slowest observed, corrects at frame 29; three seconds is two orders
 * of margin over that and still far short of anything a person would reach by
 * opening a menu. */
#define SETTLE_FRAMES 180

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

/* Raised for the warmup frames only. Their audio is pre-roll for a game that
 * has not started, produced by three unpaced calls to retro_run, and queueing
 * it is what filled the buffer before the paced loop ever ran:
 *
 *   prime to half   2048
 *   warmup frame 1  3008
 *   warmup frame 2  3968
 *   warmup frame 3  4096, and 831 of 960 frames refused
 *
 * Measured on the Brick 2026-08-25 - 4120 frames dropped at every launch,
 * identical at 300, 600, 1200 and 2400 frames of run, because it is entirely a
 * startup transient. Dropping this audio deliberately is more honest than
 * queueing it and then having the port refuse it. */
static bool g_audio_warmup;

void diatom_on_audio_batch_store(const int16_t *data, size_t frames)
{
	diatom_audio_note_input(data, frames);
	if (g_audio_warmup) return;
	diatom_audio_push(data, frames);
}

static void usage(void)
{
	int i;
	fprintf(stderr,
		"usage: diatom --core <core.so> --rom <file> [--system <dir>] [--save <dir>]\n"
		"              [--display <mode>] [--filter nearest|sharp]\n"
		"              [--load-state <file>] [--state-on-exit <file>]\n"
		"              [--firmware <name>]  required in --system, checked first\n"
		"              [--frames <n>] [--shot <file.bmp>] [--preview-on-exit <file.bmp>]\n"
		"              [--socket <path>]   launcher protocol, ADR-0009\n"
		"              [--core-option key=value] ...   repeatable\n"
		"              [--list-options]    what this core offers, then exit\n"
		"\n--firmware names a file the content needs, e.g. syscard3.pce for a PC\n"
		"Engine CD. Diatom does not know which content needs what and will not\n"
		"learn - it has no core list - so whoever launches it says (ADR-0017).\n"
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

	/* Reported from the ONE place the mode ever changes, so a launcher hears
	 * about it whether it asked, the user cycled it with a chord, or the rect
	 * settled underneath it (ADR-0021). `rect=` is free here and is what a
	 * launcher would otherwise have to recompute from geometry it does not
	 * have. ADR-0022. */
	if (diatom_proto_connected())
		diatom_proto_send("DISPLAY\tmode=%s\tfilter=%s\trect=%dx%d+%d+%d",
		                  diatom_modes[mode].name, filter_name(filter),
		                  g_dst.w, g_dst.h, g_dst.x, g_dst.y);
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

/* The in-game menu handover - ADR-0016.
 *
 * MENU pauses the game and gives the display to the launcher, which draws its
 * own menu and answers. Diatom draws nothing: it has no UI, and the handoff
 * spike proved an fbdev presenter and an EGL launcher can alternate safely so
 * long as only one presents at a time.
 *
 * Blocks until the launcher answers, because a paused game is paused - running
 * the core would advance a world the player cannot see.
 *
 * Returns false if the session should end. */
static bool menu_pause(const diatom_session *sn)
{
	/* The pause preview goes out BEFORE PAUSED, so by the time the launcher
	 * hears it owns the display, the frame it will dim and draw its menu over
	 * is already on disk. The order is the contract. ADR-0024. */
	if (sn->preview && write_preview(sn->preview))
		diatom_proto_send("PREVIEW\tpath=%s", sn->preview);

	/* The same handover as EXIT, and the same requirement: nothing in flight
	 * before the launcher starts drawing. This was a 20ms sleep, which is a
	 * guess at how long a pan takes rather than a wait for one - right often
	 * enough to look correct and wrong whenever the panel or the load
	 * disagreed.
	 *
	 * KEEP, because the launcher is about to draw its menu over the frame the
	 * player stopped on. Parking the same picture that is already showing is
	 * invisible, which is why this handover never flashed. */
	diatom_port_present_stop(DIATOM_PARK_KEEP);

	/* Symmetrical with RUNNING: the launcher may draw from here. */
	diatom_proto_send("PAUSED");

	for (;;) {
		diatom_msg m;

		switch (diatom_proto_poll(&m, -1, false)) {
		case DIATOM_MSG_RESUME:
			/* The launcher had the display and therefore owned the levels;
			 * it may have moved either while its menu was up. Re-read rather
			 * than step from a cached value nobody is at. ADR-0020. */
			diatom_port_level_invalidate();
			levels_forget();
			diatom_proto_send("RUNNING");
			return true;
		case DIATOM_MSG_STOP:
			return false;
		case DIATOM_MSG_QUIT:
			g_quit_requested = true;
			return false;
		case DIATOM_MSG_SAVE:
			diatom_proto_send(diatom_state_save(g_core, m.path)
			                  ? "SAVED\tpath=%s" : "ERROR\tcode=save_failed\tmsg=%s",
			                  m.path);
			break;
		case DIATOM_MSG_LOAD:
			diatom_proto_send(diatom_state_load(g_core, m.path)
			                  ? "LOADED\tpath=%s" : "ERROR\tcode=state_rejected\tmsg=%s",
			                  m.path);
			break;
		case DIATOM_MSG_RESET:
			/* The menu's Reset row. The game stays paused - the launcher
			 * still owns the display and sends RESUME when it is done. */
			g_core->reset();
			diatom_proto_send("RESETDONE");
			break;
		case DIATOM_MSG_OPTIONS:
			diatom_options_emit();
			break;
		case DIATOM_MSG_SETOPT:
			diatom_proto_send(diatom_options_set(m.key, m.value)
			                  ? "OPTSET\tkey=%s" : "ERROR\tcode=bad_option\tmsg=%s",
			                  m.key);
			break;
		case DIATOM_MSG_INPUTS:
		case DIATOM_MSG_MAP:
		case DIATOM_MSG_SETMAP:
		case DIATOM_MSG_LEVELS:
		case DIATOM_MSG_SETLEVEL:
			state_plane_msg(&m);
			break;
		case DIATOM_MSG_HANGUP:
			/* The launcher died while holding the menu open. Resuming is the
			 * kinder failure: the alternative strands the player in a paused
			 * game with nothing left to talk to. */
			diatom_port_log(DIATOM_LOG_WARN,
			                "launcher vanished during menu; resuming the game");
			return true;
		default:
			break;
		}
	}
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

/* ---- ADR-0020's state plane ------------------------------------------- */

static const char *const level_kind_name[DIATOM_LEVEL_COUNT] = {
	"volume", "brightness"
};
static int g_last_index[DIATOM_LEVEL_COUNT];
static int g_last_count[DIATOM_LEVEL_COUNT];

static void levels_forget(void)
{
	int k;
	for (k = 0; k < DIATOM_LEVEL_COUNT; k++) g_last_index[k] = g_last_count[k] = -1;
}

static void level_emit(int k, int idx, int cnt)
{
	g_last_index[k] = idx;
	g_last_count[k] = cnt;
	diatom_proto_send("LEVEL\tkind=%s\tindex=%d\tcount=%d",
	                  level_kind_name[k], idx, cnt);
}

/* Polled with the input bitfield and emitted on change. The port cannot send
 * anything itself - it must not know the protocol exists (ADR-0007) - so this
 * is the whole path by which a volume press during a game reaches a launcher
 * that is not drawing and cannot see it. */
static void levels_tick(void)
{
	int k, idx, cnt;

	if (!diatom_proto_connected()) return;
	for (k = 0; k < DIATOM_LEVEL_COUNT; k++) {
		if (!diatom_port_level_get((diatom_level_kind)k, &idx, &cnt)) continue;
		if (idx == g_last_index[k] && cnt == g_last_count[k]) continue;
		level_emit(k, idx, cnt);
	}
}

static void levels_emit_all(void)
{
	int k, idx, cnt, n = 0;

	for (k = 0; k < DIATOM_LEVEL_COUNT; k++)
		if (diatom_port_level_get((diatom_level_kind)k, &idx, &cnt)) n++;

	/* count=0 is the answer on a port with no level control of its own, and
	 * says "expect no events" instead of leaving it to be inferred. */
	diatom_proto_send("LEVELS\tcount=%d", n);
	for (k = 0; k < DIATOM_LEVEL_COUNT; k++)
		if (diatom_port_level_get((diatom_level_kind)k, &idx, &cnt))
			level_emit(k, idx, cnt);
}

static void level_set(const diatom_msg *m)
{
	int k, idx, cnt;

	for (k = 0; k < DIATOM_LEVEL_COUNT; k++)
		if (!strcmp(level_kind_name[k], m->lkind)) break;

	if (k == DIATOM_LEVEL_COUNT || m->count <= 0 ||
	    !diatom_port_level_set((diatom_level_kind)k, m->index, m->count)) {
		diatom_proto_send("ERROR\tcode=bad_level\tmsg=%s", m->lkind);
		return;
	}
	/* Answer in OUR positions rather than echoing theirs. The launcher sent a
	 * fraction of its own ladder and needs to know which rung it landed on. */
	if (diatom_port_level_get((diatom_level_kind)k, &idx, &cnt))
		level_emit(k, idx, cnt);
}

static void display_set(const diatom_msg *m)
{
	int mode = g_mode, i;
	diatom_filter filter = g_filter;

	if (m->dmode[0]) {
		for (i = 0; i < diatom_mode_count; i++)
			if (!strcmp(diatom_modes[i].name, m->dmode)) break;
		if (i == diatom_mode_count) {
			diatom_proto_send("ERROR\tcode=bad_display\tmsg=%s", m->dmode);
			return;
		}
		mode = i;
	}
	if (m->dfilter[0]) {
		if      (!strcmp(m->dfilter, "nearest")) filter = DIATOM_FILTER_NEAREST;
		else if (!strcmp(m->dfilter, "sharp"))   filter = DIATOM_FILTER_SHARP;
		else {
			diatom_proto_send("ERROR\tcode=bad_display\tmsg=%s", m->dfilter);
			return;
		}
	}
	/* Both fields validated before either is applied, for the same reason
	 * SETMAP is all-or-nothing: a half-applied setting is one nobody asked
	 * for and neither side believes in. */
	apply_display(mode, filter);
}

/* Shared by all three message loops. A launcher may read or write any of this
 * whenever it likes: the two moments it is drawing - menu and idle - are
 * exactly the moments Diatom is not, and it is no less valid mid-game. */
static bool state_plane_msg(const diatom_msg *m)
{
	switch (m->kind) {
	case DIATOM_MSG_INPUTS: diatom_input_emit_labels(); return true;
	case DIATOM_MSG_MAP:    diatom_input_emit_map();    return true;
	case DIATOM_MSG_SETMAP:
		if (!diatom_input_set_map(m->map))
			diatom_proto_send("ERROR\tcode=bad_map\tmsg=%s", m->map);
		/* Answered either way, so a refusal cannot leave the launcher
		 * believing a map it does not have. */
		diatom_input_emit_map();
		return true;
	case DIATOM_MSG_LEVELS:   levels_emit_all(); return true;
	case DIATOM_MSG_SETLEVEL: level_set(m);      return true;
	case DIATOM_MSG_DISPLAY:
		/* apply_display is what emits, so ask it to restate the current one
		 * rather than growing a second path that could disagree with it. */
		apply_display(g_mode, g_filter);
		return true;
	case DIATOM_MSG_SETDISPLAY: display_set(m); return true;
	default: return false;
	}
}

static int run_session_inner(const diatom_session *sn)
{
	struct retro_system_av_info av;
	struct retro_system_info si;
	double   frame_us, next_us;
	uint64_t t_start;
	uint32_t buttons = 0, prev_buttons = 0;
	long     locked_at = 0;     /* frame the current rect was computed on */

	/* Every game starts from identity and from an unknown level, so a launcher
	 * that sends no map gets no map, and the first poll reports where the
	 * levels actually are without being asked. ADR-0020. */
	diatom_input_reset_map();
	levels_forget();
	diatom_port_level_invalidate();
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
	/* Firmware, checked BEFORE the core is asked to load - ADR-0017.
	 *
	 * Diatom does not know that a PC Engine CD needs `syscard3.pce`, and will
	 * not learn: it has no core list, and a firmware table is a core list by
	 * another name. The launcher already maps system to core, so it is the
	 * thing that knows, and it says so with `firmware=`.
	 *
	 * Checked here rather than after a failed load because the distinction
	 * ADR-0009 draws is about the DISPLAY. A missing System Card means the game
	 * never started, so the launcher must keep drawing and hear ERROR. The core
	 * only ever tells us `retro_load_game` returned false, which is the same
	 * answer it gives for a corrupt ROM. */
	if (sn->firmware && *sn->firmware) {
		char path[1024];
		snprintf(path, sizeof path, "%s/%s", g_policy.system_dir, sn->firmware);
		if (access(path, R_OK) != 0) {
			fprintf(stderr, "diatom: missing firmware %s\n", path);
			diatom_proto_send("ERROR\tcode=bios_missing\tmsg=%s", sn->firmware);
			return 4;
		}
	}

	if (!diatom_core_start(g_core, sn->rom)) {
		/* Say the firmware was found, so a launcher that reports this does not
		 * send someone hunting for a BIOS they already have. */
		if (sn->firmware && *sn->firmware)
			diatom_proto_send("ERROR\tcode=rom_unreadable"
			                  "\tmsg=core refused the rom (%s was present)",
			                  sn->firmware);
		else
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
	locked_at = 0;

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
	g_phase = PHASE_RUNNING;   /* a crash from here on is EXIT, not ERROR */

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
		g_audio_warmup = true;
		for (w = 0; w < 3; w++) {
			g_core->run();
			diatom_port_present(g_frame, g_frame_w, g_frame_h, g_frame_pitch,
			                    g_policy.pixfmt, g_dst,
			                    g_filter);
		}
		g_audio_warmup = false;
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

		/* ADR-0011 locked the rect and never moved it. That is right when the
		 * load-time geometry is the mode the game runs in and the change is an
		 * excursion - SNES and PC Engine hires - and wrong when the load-time
		 * value is a boot artefact.
		 *
		 * Measured 2026-08-26 with tools/envlog.c over 3600 frames:
		 *
		 *   genesis_plus_gx  Herzog Zwei       256x192 for 29 frames, then
		 *                                      320x224 for the other 3571
		 *                    Phantasy Star IV  256x192 for ONE frame
		 *   snes9x2010       four games        256x224 for all 3600
		 *   mednafen_pce_fast three games      256x243 for all 3600
		 *
		 * So the discriminator is not what changed, it is whether what we
		 * locked onto ever really held: relock only if the geometry we are
		 * displaying turned out to be transient. Genesis corrects itself
		 * within half a second, during the boot logo. A hires menu opened
		 * minutes in does not qualify and the rect stays put, which is the
		 * behaviour ADR-0011 exists to protect. */
		if (diatom_env_geometry_changed()) {
			int nw, nh;
			double na;

			geom_changes++;
			if (frames - locked_at < SETTLE_FRAMES &&
			    diatom_env_new_geometry(&nw, &nh, &na) &&
			    (nw != g_base_w || nh != g_base_h)) {
				printf("diatom: geometry settled %dx%d -> %dx%d "
				       "at frame %ld\n",
				       g_base_w, g_base_h, nw, nh, frames);
				/* Reprinted in the SAME format as the load-time
				 * line so anything parsing that format sees the
				 * mode the game actually runs in by taking the
				 * last one. tools/corefacts.sh took the first and
				 * recorded Genesis as 256x192 at 1.5238 - twenty-
				 * nine frames of boot - which ADR-0018's display
				 * table was then computed from. */
				printf("diatom: %dx%d (max %ux%u) aspect %.4f, "
				       "%.4f fps, %.0f Hz -> %d Hz\n",
				       nw, nh, av.geometry.max_width,
				       av.geometry.max_height, na,
				       av.timing.fps, av.timing.sample_rate,
				       g_caps.audio_rate);
				g_base_w      = nw;
				g_base_h      = nh;
				g_base_aspect = na;
				apply_display(g_mode, g_filter);
				locked_at = frames;
			}
		}

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
			case DIATOM_MSG_OPTIONS: diatom_options_emit(); break;
			case DIATOM_MSG_SETOPT:
				diatom_proto_send(diatom_options_set(m.key, m.value)
				                  ? "OPTSET\tkey=%s"
				                  : "ERROR\tcode=bad_option\tmsg=%s", m.key);
				break;
			/* Also honoured while running, not only from the menu. ADR-0016
			 * puts slot CHOICE in the launcher's menu, but nothing about that
			 * requires the game to be paused to write a state. */
			case DIATOM_MSG_SAVE:
				diatom_proto_send(diatom_state_save(g_core, m.path)
				                  ? "SAVED\tpath=%s"
				                  : "ERROR\tcode=save_failed\tmsg=%s", m.path);
				break;
			case DIATOM_MSG_LOAD:
				diatom_proto_send(diatom_state_load(g_core, m.path)
				                  ? "LOADED\tpath=%s"
				                  : "ERROR\tcode=state_rejected\tmsg=%s", m.path);
				break;
			case DIATOM_MSG_RESET:
				g_core->reset();
				diatom_proto_send("RESETDONE");
				break;
			/* HANGUP is deliberately not a stop. ADR-0008 exists so a
			 * launcher that died cannot take a running game with it. */
			default: state_plane_msg(&m); break;
			}
		}

		levels_tick();

		buttons = diatom_port_input_state();
		display_chord(buttons, prev_buttons);

		/* MENU is Diatom's own key and the ports no longer act on it, because
		 * what it means is host policy: standalone it ends the session, under
		 * the launcher it opens the launcher's menu. Edge-triggered, or holding
		 * it would re-enter the menu every frame. */
		if ((buttons & ~prev_buttons) & DIATOM_BIT(DIATOM_BTN_MENU)) {
			if (!diatom_proto_active()) {
				stop = true;
			} else if (!menu_pause(sn)) {
				stop = true;
			} else {
				/* The clock ran on while the menu was open. Without this the
				 * loop believes it is thousands of frames late and spends the
				 * next second catching up. */
				next_us = (double)diatom_port_now_us();

				/* Re-read input and treat it as already-seen, so MENU must be
				 * RELEASED before it can open the menu again.
				 *
				 * Clearing prev_buttons instead looks equivalent and is not:
				 * the pause happens the instant the key goes down, so a finger
				 * is still on it when the launcher resumes, and the next frame
				 * reads that as a fresh press. Measured with a human 2026-08-25
				 * - one press produced two menus. */
				diatom_port_input_poll();
				prev_buttons = diatom_port_input_state();
				continue;
			}
		}
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

	/* The frame the player was looking at, for the launcher's card - written
	 * with the exit state so a card that shows a preview always has a state
	 * to resume, and announced before EXIT so it is on disk before the
	 * launcher redraws. ADR-0024. */
	if (sn->preview && write_preview(sn->preview))
		diatom_proto_send("PREVIEW\tpath=%s", sn->preview);

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
		printf("diatom: audio dropped %llu frame(s)\n",
		       (unsigned long long)diatom_audio_dropped());
		printf("diatom: audio IN  peak %d rms %.0f, %llu of %llu non-zero\n",
		       diatom_audio_in_peak(), diatom_audio_in_rms(),
		       (unsigned long long)diatom_audio_in_nonzero(),
		       (unsigned long long)diatom_audio_in_samples());
		printf("diatom: audio OUT peak %d rms %.0f (%.1f%% fs), "
		       "%llu of %llu samples non-zero\n",
		       diatom_audio_peak(), diatom_audio_rms(), diatom_audio_peak() * 100.0 / 32767.0,
		       (unsigned long long)diatom_audio_nonzero(),
		       (unsigned long long)diatom_audio_samples());
		printf("diatom: audio queued min %zu max %zu final %zu, target %d, capacity %d\n",
		       q_min == (size_t)-1 ? 0 : q_min, q_max,
		       diatom_port_audio_queued(),
		       g_caps.audio_buffer_frames / 2, g_caps.audio_buffer_frames);
		printf("diatom: rate control drift %+.3f%% (max %+.3f%%)\n",
		       diatom_audio_ratio_drift() * 100.0, 0.5);
	}

	diatom_core_stop(g_core);
	/* No dlclose. Ever. ADR-0006. */

	/* Stop presenting BEFORE saying so. The launcher begins drawing the moment
	 * it reads EXIT, and until this call returned the flip thread could still
	 * have a pan in flight - two processes driving one framebuffer, which
	 * measured from the launcher side as a boundary sweeping down the panel
	 * over several frames. Not a wedge, because the window is milliseconds,
	 * but the contract below is only true once this has returned.
	 *
	 * BLANK, and the asymmetry with the pause above is the whole point. This
	 * park replaces whatever is on glass, and at an exit that is often the
	 * launcher's own in-game menu - the player quit from it. Parking the last
	 * frame there showed a bare game frame, without the menu, for the few tens
	 * of milliseconds before the shelf faded up: a flash on quit and never on
	 * opening the menu, which is exactly the asymmetry that identified it. The
	 * launcher fades up from black, so black is the state it is about to
	 * assume anyway. */
	diatom_port_present_stop(DIATOM_PARK_BLANK);

	/* The game ran and stopped, which is EXIT rather than ERROR whatever the
	 * reason. The launcher may take the display back now.
	 *
	 * Always `user`, and that is not a placeholder. Every way of arriving here
	 * is someone deciding to stop: STOP or QUIT from the launcher, MENU when
	 * standalone, a closed window, or SIGTERM - which on the Brick is the power
	 * button, so still the player. The other two reasons ADR-0009 lists are
	 * produced elsewhere: `crash` by the handler above, since a crash never
	 * reaches this line, and `error` by nothing yet - there is no post-RUNNING
	 * failure Diatom can currently detect that is not a crash. */
	diatom_proto_send("EXIT\treason=user");
	return 0;
}

/* The phase belongs out here, not inside, so that an early return added later
 * cannot forget to clear it and leave the crash handler reporting on a game
 * that is no longer in flight. There are six such returns already. */
static int run_session(const diatom_session *sn)
{
	int rc;

	g_phase = PHASE_LOADING;
	rc = run_session_inner(sn);
	g_phase = PHASE_IDLE;
	return rc;
}

int main(int argc, char **argv)
{
	const char *core_path = NULL, *rom_path = NULL, *shot_path = NULL;
	/* ADR-0014: stretch by default, judged on the panel. A handheld's screen
	 * is its whole interface, and full use of it beat both the letterbox and
	 * the crop. nearest because sharp earned nothing visible at these factors
	 * and is the only thing that has made this loop miss a frame. */
	const char *display = "stretch", *filter = "nearest";
	const char *state_load = NULL, *state_exit = NULL, *firmware = NULL, *tap = NULL;
	const char *preview_path = NULL;
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
		else if (!strcmp(argv[i], "--preview-on-exit") && i + 1 < argc) preview_path = argv[++i];
		else if (!strcmp(argv[i], "--firmware") && i + 1 < argc) firmware = argv[++i];
		else if (!strcmp(argv[i], "--tap-audio") && i + 1 < argc) tap = argv[++i];
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

	/* Reflect the process default in the live state, AFTER both are parsed.
	 *
	 * g_mode is a static and defaults to 0, which is diatom_modes[0] -
	 * `integer`. The real default is `stretch`, and it was only ever applied
	 * once a session started. Harmless while nothing could ask; ADR-0022 lets
	 * a launcher query DISPLAY on an idle Diatom, and it answered
	 * `mode=integer` for a frontend that would have used `stretch`. Caught on
	 * hardware 2026-08-26 by asking with no game loaded. */
	g_mode   = start_mode;
	g_filter = start_filter;

	if (!g_policy.system_dir) g_policy.system_dir = ".";
	if (!g_policy.save_dir)   g_policy.save_dir   = ".";
	g_policy.pixfmt = DIATOM_PIX_RGB565;   /* libretro's default is 0RGB1555,
	                                          which Diatom refuses; every core
	                                          measured picked RGB565 anyway */

	/* Line-buffered, so a crash cannot swallow the log that explains it.
	 * Redirected to a file stdout is block-buffered, and the crash runs on
	 * 2026-08-25 lost everything since the last 4 KB boundary - including the
	 * warmup line, which made a session look like it had never got that far.
	 * Costs one write per line, and Diatom prints tens of lines per session,
	 * not per frame. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (!diatom_port_init(&g_caps)) {
		fprintf(stderr, "diatom: port init failed\n");
		return 2;
	}

	/* Once for the life of the process, not per session: the first thing a
	 * session does is dlopen and start a core, and a core that dies there is
	 * exactly the case worth reporting. */
	install_crash_handlers();

	if (tap) {
		char a[512], b[512];
		snprintf(a, sizeof a, "%s.in.raw",  tap);
		snprintf(b, sizeof b, "%s.out.raw", tap);
		diatom_audio_tap(a, b);
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
			switch (diatom_proto_poll(&m, -1, false)) {
			case DIATOM_MSG_RUN:     break;
			case DIATOM_MSG_OPTIONS: diatom_options_emit(); continue;
			case DIATOM_MSG_SETOPT:  diatom_options_set(m.key, m.value); continue;
			/* QUIT was falling through to `continue` here, so an IDLE Diatom
			 * ignored it and only a running one could be shut down - the exact
			 * opposite of what a launcher needs, since between games is when it
			 * would ask. Found 2026-08-25 while testing the crash paths: every
			 * run ended on the watchdog's SIGKILL rather than on QUIT. */
			case DIATOM_MSG_QUIT:    g_quit_requested = true; continue;
			default:                 state_plane_msg(&m); continue;
			}
			if (!m.core[0]) {
				diatom_proto_send("ERROR\tcode=core_missing\tmsg=no core given");
				continue;
			}

			memset(&sn, 0, sizeof sn);
			sn.core     = m.core;
			sn.rom      = m.rom[0] ? m.rom : NULL;
			sn.firmware = m.firmware[0] ? m.firmware : NULL;
			/* ADR-0024: the launcher owns where things persist and says so
			 * per game. resume is load-if-exists - a missing state is a
			 * fresh start, not an error, which is what makes it safe to
			 * pass unconditionally. */
			sn.state_load = m.resume[0]     ? m.resume     : NULL;
			sn.state_exit = m.exit_state[0] ? m.exit_state : NULL;
			sn.preview    = m.preview[0]    ? m.preview    : NULL;
			sn.mode   = start_mode;
			sn.filter = start_filter;
			run_session(&sn);   /* its own ERROR/EXIT is the report */
		}
		diatom_proto_close();
	} else {
		diatom_session sn = { core_path, rom_path, shot_path,
		                      state_load, state_exit, preview_path, firmware,
		                      limit, start_mode, start_filter,
		                      list_options };
		int rc = run_session(&sn);
		if (rc) return rc;
	}
	diatom_audio_tap_close();
	diatom_port_shutdown();
	free(g_frame);
	return 0;
}
