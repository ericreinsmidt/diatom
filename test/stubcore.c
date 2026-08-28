/* A libretro core that is not an emulator.
 *
 * Exists so Diatom can be developed and tested with no third-party binary
 * present. It implements the API and draws a test pattern, which is enough to
 * exercise every path the frontend has: dlopen and symbol binding, the
 * environment callback, the copy-then-present video path, integer scaling,
 * resampling, input, and pacing.
 *
 * It deliberately reproduces the awkward cases the env-inventory spike measured
 * in real cores, because those are exactly what a happy-path stub would hide:
 *
 *   - 59.7275 fps, not 60      : no console runs at 60, so pacing must not assume it
 *   - 32040 Hz sample rate     : forces the resampler to actually resample
 *   - SET_GEOMETRY mid-run     : 3 of 6 measured cores change geometry while running
 *   - RGB565                   : what all six chose when offered a choice
 *
 * It can also be told to die, which is the only way to test the crash side of
 * the protocol - `EXIT reason=crash` versus `ERROR code=crash`. That belongs
 * here rather than in Diatom: a frontend with a --crash flag would be test code
 * living in the shipped binary, and the whole point is that Diatom learns about
 * the crash the same way it will in the field, from a signal it did not raise.
 *
 *   STUBCORE_CRASH=segv[@N]   null dereference, at frame N (default 60)
 *   STUBCORE_CRASH=stack[@N]  unbounded recursion - needs the altstack
 *   STUBCORE_CRASH=abort[@N]  abort(), so SIGABRT
 *   STUBCORE_CRASH=fpe[@N]    integer divide by zero - a NO-OP on ARM, which
 *                             does not trap it; kept because it does trap on
 *                             x86 and the difference is worth being able to see
 *   STUBCORE_CRASH=exit[@N]   exit(1), which raises no signal at all
 *   STUBCORE_CRASH=load       die inside retro_load_game, before RUNNING
 *
 * Test fixture. Not part of Diatom's runtime.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "libretro.h"

#define BASE_W 256
#define BASE_H 224
#define WIDE_W 512          /* the "hires" mode, as SNES and PC Engine do */
#define MAX_W  512
#define MAX_H  448
#define FPS    59.7275
#define RATE   32040.0
/* 32040/59.7275 = 536.44, so 1024 is ample headroom. */
#define AUDIO_MAX_FRAMES 1024

static retro_environment_t   env;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_cb;
static retro_input_poll_t    poll_cb;
static retro_input_state_t   input_cb;

static uint16_t fb[MAX_W * MAX_H];
static int      cur_w = BASE_W, cur_h = BASE_H;
static unsigned frame;
static int      box_x = 96, box_y = 80;
static double   phase;
static double   audio_accum;

void retro_set_environment(retro_environment_t cb)
{
	bool no_game = true;
	env = cb;
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
}
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb)   { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb)       { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb)     { input_cb = cb; }

void retro_init(void) { frame = 0; }
void retro_deinit(void) { }
unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info)
{
	memset(info, 0, sizeof *info);
	info->library_name     = "diatom-stub";
	info->library_version  = "1";
	info->valid_extensions = "stub";
	info->need_fullpath    = false;
	info->block_extract    = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
	memset(info, 0, sizeof *info);
	info->geometry.base_width   = cur_w;
	info->geometry.base_height  = cur_h;
	info->geometry.max_width    = MAX_W;
	info->geometry.max_height   = MAX_H;
	info->geometry.aspect_ratio = 4.0f / 3.0f;
	info->timing.fps            = FPS;
	info->timing.sample_rate    = RATE;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{ (void)port; (void)device; }

/* Deliberate faults. `volatile` throughout and no inlining, because -O2 is
 * entitled to assume undefined behavior never happens: an unguarded null store
 * can be deleted outright, and a self-call with no side effect can be turned
 * into a loop that never grows the stack. Both were observed while writing
 * this - the segv mode compiled to nothing at all. */
static volatile int *const null_ptr = (volatile int *)0;

/* Somewhere for a frame address to go, so the frame cannot be discarded. */
static volatile char *frame_sink;

__attribute__((noinline)) static int recurse(volatile int depth)
{
	volatile char eat[512];
	int r;

	eat[0] = (char)depth;
	/* The bound is for the compiler, not for correctness: without it this is
	 * provably infinite and -Winfinite-recursion fires, and a build that warns
	 * on purpose is a build whose warnings get ignored. 2^30 frames of 512
	 * bytes is half a terabyte of stack, so the guard never runs. */
	if (depth > (1 << 30)) return eat[0];

	r = recurse(depth + 1);
	/* Touching the frame AFTER the call is what makes this recursion. `volatile`
	 * and `noinline` are not enough on their own: aarch64-linux-gnu-gcc at -O2
	 * turned the obvious version into `add sp, sp, #0x210; b recurse` - a
	 * sibling call that pops its frame before branching, so the stack never
	 * grew and the fixture quietly tested nothing. It still crashed on macOS,
	 * which is how the disagreement surfaced. Taking the frame's address here
	 * means the frame must outlive the call, and a tail call becomes illegal. */
	frame_sink = &eat[0];
	return r + eat[0];
}

static void die(const char *how)
{
	if (!strcmp(how, "segv"))  { *null_ptr = 1; }
	if (!strcmp(how, "stack")) { recurse(0); }
	if (!strcmp(how, "abort")) { abort(); }
	if (!strcmp(how, "exit"))  { exit(1); }
	if (!strcmp(how, "fpe"))   { volatile int z = 0; volatile int r = 1 / z; (void)r; }
}

/* `mode` is the word before any '@', `at` the frame after it. */
static char     crash_mode[16];
static unsigned crash_at = 60;

static void crash_configure(void)
{
	const char *e = getenv("STUBCORE_CRASH");
	const char *at;
	size_t n;

	if (!e || !*e) return;
	at = strchr(e, '@');
	n  = at ? (size_t)(at - e) : strlen(e);
	if (n >= sizeof crash_mode) n = sizeof crash_mode - 1;
	memcpy(crash_mode, e, n);
	crash_mode[n] = '\0';
	if (at) crash_at = (unsigned)strtoul(at + 1, NULL, 10);
}

/* Deliberately partial and deliberately not in enum order: real cores describe
 * only the buttons a game uses, so a frontend that assumed a full array or an
 * ordered one would pass here and fail on hardware. */
static const struct retro_input_descriptor descriptors[] = {
	{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Fire" },
	{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Jump" },
	{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Pause" },
	{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "P2 Fire" },
	{ 0 }
};

bool retro_load_game(const struct retro_game_info *game)
{
	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
	(void)game;                                  /* content is optional here */
	crash_configure();
	/* Offered by every real core and discarded by Diatom until ADR-0020. The
	 * port-1 entry is here so the "port 0 only" filter has something to
	 * exclude rather than being untested. */
	if (env) env(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)descriptors);
	/* Before RUNNING has gone out, so the frontend owes the launcher an ERROR
	 * and must keep its hands off the display. */
	if (!strcmp(crash_mode, "load")) *null_ptr = 1;
	return env && env(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
}
bool retro_load_game_special(unsigned t, const struct retro_game_info *i, size_t n)
{ (void)t; (void)i; (void)n; return false; }
void retro_unload_game(void) { }
unsigned retro_get_region(void) { return RETRO_REGION_PAL; }

size_t retro_serialize_size(void) { return sizeof frame; }
bool retro_serialize(void *d, size_t n)
{ if (n < sizeof frame) return false; memcpy(d, &frame, sizeof frame); return true; }
bool retro_unserialize(const void *d, size_t n)
{ if (n < sizeof frame) return false; memcpy(&frame, d, sizeof frame); return true; }

void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
void retro_reset(void) { frame = 0; }
void retro_cheat_reset(void) { }
void retro_cheat_set(unsigned i, bool e, const char *c)
{ (void)i; (void)e; (void)c; }

static uint16_t rgb565(int r, int g, int b)
{
	return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

void retro_run(void)
{
	int x, y, xs;
	int16_t audio[AUDIO_MAX_FRAMES * 2];
	size_t n, i;
	int16_t held;

	/* Die from inside retro_run, on a frame late enough that RUNNING has long
	 * gone out and the frontend is in its steady loop. Doing it on frame 0
	 * would land during the warmup and prove less. */
	if (crash_mode[0] && frame == crash_at) die(crash_mode);

	/* One frame's worth of audio, with the fraction carried.
	 *
	 * 32040/59.7275 is 536.44 frames per video frame. An earlier version of this
	 * clamped to 512 to fit a smaller buffer, which under-produced by 4.6% -
	 * nine times what rate control can correct - and starved the frontend's audio
	 * queue. The frontend was behaving correctly on a starved input; the fixture
	 * was lying. A test core that produces the wrong amount of audio tests
	 * nothing useful. */
	audio_accum += RATE / FPS;
	n = (size_t)audio_accum;
	audio_accum -= (double)n;
	if (n > AUDIO_MAX_FRAMES) n = AUDIO_MAX_FRAMES;   /* cannot happen; guard anyway */

	poll_cb();

	/* A box you can drive, so input is verifiable by looking at it. */
	held = input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
	if (held) box_x -= 2;
	if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT)) box_x += 2;
	if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))    box_y -= 2;
	if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))  box_y += 2;

	/* Toggle between base and hires every 5 seconds, the way a real core does
	 * when a game opens a menu. This is the path 3 of 6 measured cores take and
	 * the one most likely to be broken by an implementation that computes the
	 * destination rect once at load. */
	if (frame && frame % 300 == 0) {
		struct retro_game_geometry g;
		cur_w = (cur_w == BASE_W) ? WIDE_W : BASE_W;
		memset(&g, 0, sizeof g);
		g.base_width   = cur_w;
		g.base_height  = cur_h;
		g.max_width    = MAX_W;
		g.max_height   = MAX_H;
		g.aspect_ratio = 4.0f / 3.0f;
		env(RETRO_ENVIRONMENT_SET_GEOMETRY, &g);
	}

	/* Draw in DISPLAY units, not source pixels.
	 *
	 * Hires pixels are physically half-width, so real content in a 512-wide mode
	 * uses 512-wide coordinates and looks the same size as it did at 256 - just
	 * with finer detail. A fixture that draws a 32-source-pixel box in both
	 * modes renders correctly and demonstrates nothing, because the box really
	 * is half as wide in hires. Scaling x by this factor is what makes the
	 * comparison honest. */
	xs = cur_w / BASE_W;                 /* 1 at base, 2 at hires */

	if (box_x < 0) box_x = 0;
	if (box_y < 0) box_y = 0;
	if (box_x > BASE_W - 32) box_x = BASE_W - 32;
	if (box_y > cur_h - 32) box_y = cur_h - 32;

	for (y = 0; y < cur_h; y++) {
		for (x = 0; x < cur_w; x++) {
			int v = ((x / xs + frame) / 16 + (y / 16)) & 1;
			fb[y * cur_w + x] = v ? rgb565(24, 24, 32) : rgb565(48, 52, 64);
		}
	}

	/* Detail that only resolves in hires: vertical lines one SOURCE pixel wide.
	 * At base these are 3 screen pixels apart, at hires 1.5 - which is the whole
	 * point of the mode, and the thing a correct implementation should show. */
	for (y = 24; y < 72 && y < cur_h; y++)
		for (x = 24 * xs; x < 120 * xs && x < cur_w; x += 2)
			fb[y * cur_w + x] = rgb565(200, 200, 210);
	/* Single-pixel border, so integer scaling is checkable by eye: at 3x each
	 * edge should be exactly three screen pixels, with no smearing. */
	for (x = 0; x < cur_w; x++) {
		fb[x] = rgb565(255, 255, 255);
		fb[(cur_h - 1) * cur_w + x] = rgb565(255, 255, 255);
	}
	for (y = 0; y < cur_h; y++) {
		fb[y * cur_w] = rgb565(255, 255, 255);
		fb[y * cur_w + cur_w - 1] = rgb565(255, 255, 255);
	}
	for (y = box_y; y < box_y + 32 && y < cur_h; y++)
		for (x = box_x * xs; x < (box_x + 32) * xs && x < cur_w; x++)
			fb[y * cur_w + x] = rgb565(220, 90, 40);

	video_cb(fb, (unsigned)cur_w, (unsigned)cur_h, (size_t)cur_w * sizeof(uint16_t));

	for (i = 0; i < n; i++) {
		int16_t s = (int16_t)(sin(phase) * 2200.0);
		audio[i * 2] = audio[i * 2 + 1] = s;
		phase += 2.0 * M_PI * 440.0 / RATE;
		if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
	}
	audio_cb(audio, n);

	frame++;
}
