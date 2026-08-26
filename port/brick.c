/* TrimUI Brick (TG3040) port.
 *
 * Presentation is raw fbdev with a flip thread; SDL2 (the firmware's own
 * library, via the sysroot - ADR-0012) provides audio, joystick input and the
 * monotonic clock.
 *
 * fbdev is not a fallback, it is the device's native display path. Measured
 * 2026-08-24 on firmware 1.1.1: scanout is the Allwinner disp2 engine
 * (/dev/fb0 -> the "disp" platform driver), while /dev/dri/card0 is only the
 * PowerVR render node (pvrsrvkm) with no display capability. The firmware's
 * SDL2 has exactly one real video driver, "mali", whose EGL swap blocks
 * ~30ms - two vblank intervals - regardless of swap interval, which quantised
 * the frame loop to 30fps.
 *
 * FBIOPAN_DISPLAY is itself a blocking vsync'd flip. Measured: a pan issued
 * right after vblank returns in one interval (a tight pan loop sustains
 * 60fps), but one issued mid-interval misses the latch deadline and waits for
 * the vsync after next (~25ms). A self-paced loop always lands mid-interval,
 * so calling pan inline halved the frame rate exactly as the EGL swap did.
 *
 * Hence the flip thread. present() only blits and publishes the page in a
 * latest-wins mailbox, never blocking, which is the seam's contract; the
 * thread pans at the panel's own rate and eats the blocking wait. Flips latch
 * at vblank, so no tearing; three pages mean the page being drawn is never
 * the one on glass or in flight. The frame loop keeps its own absolute clock,
 * exactly as on desktop - no console runs at the panel's rate.
 *
 * The firmware keeps its SDL2 outside the default linker path, so run with:
 *
 *   LD_LIBRARY_PATH=/usr/trimui/lib ./diatom --core X.so --rom game
 *
 * This file does not include libretro.h and must never need to.
 */
#include <SDL.h>

#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "diatom_port.h"

#define AUDIO_RATE 48000
/* Capacity in FRAMES (one frame = two int16 samples). 4096 at 48kHz is ~85ms,
 * with rate control aiming to hold it near half that. Same figure as desktop:
 * nothing about the device argues for a different one yet. */
#define AUDIO_BUFFER_FRAMES 4096
#define AUDIO_FRAME_BYTES   (2 * (int)sizeof(int16_t))

#define FB_PAGES 3

static int                       g_fb_fd = -1;
static uint8_t                  *g_fb;
static size_t                    g_fb_size;
static struct fb_var_screeninfo  g_vinfo;
static struct fb_fix_screeninfo  g_finfo;
static int                       g_pages;      /* usable pages, up to FB_PAGES */
static bool                      g_pan_broken; /* pan failed; draw to front */

/* Flip mailbox. All page-role fields are guarded by g_flip_mx:
 *   g_front    on glass (last pan that completed)
 *   g_inflight being panned right now, -1 if none
 *   g_pending  published by present(), waiting for the thread, -1 if none
 * present() draws into any page holding none of those roles; when all three
 * are taken it steals g_pending back, which is safe precisely because the
 * thread only takes pending under the same lock. Latest wins, nothing waits. */
static pthread_t        g_flip_thread;
static pthread_mutex_t  g_flip_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_flip_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t   g_flip_idle = PTHREAD_COND_INITIALIZER;
static int              g_front;
static int              g_inflight = -1;
static int              g_pending  = -1;
static bool             g_flip_stop;
static bool             g_flip_running;

static SDL_AudioDeviceID g_audio;
static SDL_Joystick     *g_joy;
static bool              g_quit;
static uint32_t          g_buttons;
static bool              g_input_debug;
static bool              g_present_debug;

/* Opaque value for the framebuffer's alpha channel, zero if it has none.
 * The disp2 engine composites the fb layer in PER-PIXEL alpha mode: pixels
 * with a zero alpha byte are invisible, composited over black. Measured the
 * hard way 2026-08-24 - a pipeline that paced and captured perfectly while
 * the panel showed nothing, because capture reads memory and the panel reads
 * alpha. */
static uint32_t g_opaque;

/* Resampling maps, one entry per destination pixel on each axis, rebuilt only
 * when the source size, the destination rect or the filter changes.
 *
 * `w` is the weight of source pixel idx+1, in 1/256 units. Zero means the
 * destination pixel sits wholly inside one source pixel, which is the case for
 * every pixel at an integer factor and for most of them otherwise - so it is
 * the fast path, not an optimisation for a rare case. */
typedef struct { int idx; int w; } diatom_tap;
typedef struct { int r, g, b; }    diatom_rgb;

static diatom_tap  *g_colmap, *g_rowmap;
static int          g_map_src_w = -1, g_map_src_h = -1;
static diatom_rect  g_map_dst;
static diatom_filter g_map_filter;
static bool          g_map_valid;

/* Output gain, by ioctl on /dev/snd/controlC0.
 *
 * No alsa-lib or tinyalsa in the sysroot, and forking `tinymix` per keypress
 * means a process spawn in the input path plus a dependency on a firmware
 * binary. So: a raw ioctl on a device node, which is what this port already
 * does for /dev/fb0. The struct layout is vendored from the kernel UAPI rather
 * than depended on, and its SIZE is baked into the request number by _IOWR -
 * validated against the running kernel by tools/mixprobe.c before it was
 * written here (sizeof 1224, read agreed with tinymix to the digit).
 *
 * `digital volume` is 0-63 and **INVERTED**: 0 is loudest, 63 is silence. The
 * driver advertises `dBscale-min=-74.24dB, step=+1.16dB`, i.e. that higher is
 * louder. That metadata is wrong. Proven by diffing the mixer across a
 * volume-up press in the device UI: 37 -> 15 when turned UP.
 *
 * `Headphone Volume` is deliberately untouched. It is not a speaker level -
 * raising it routes output to the headphone JACK and mutes the speakers.
 */
struct dm_ctl_elem_id {
	unsigned int numid; int iface; unsigned int device, subdevice;
	unsigned char name[44]; unsigned int index;
};
struct dm_aes_iec958 {
	unsigned char status[24], subcode[147], pad, dig_subframe[4];
};
struct dm_ctl_elem_value {
	struct dm_ctl_elem_id id;
	unsigned int indirect: 1;
	union {
		union { long value[128]; long *value_ptr; } integer;
		union { long long value[64]; long long *value_ptr; } integer64;
		union { unsigned int item[128]; unsigned int *item_ptr; } enumerated;
		union { unsigned char data[512]; unsigned char *data_ptr; } bytes;
		struct dm_aes_iec958 iec958;
	} value;
	unsigned char reserved[128];
};
#define DM_CTL_ELEM_READ   _IOWR('U', 0x12, struct dm_ctl_elem_value)
#define DM_CTL_ELEM_WRITE  _IOWR('U', 0x13, struct dm_ctl_elem_value)

#define GAIN_CTL     "digital volume"
#define GAIN_RAW_MAX 63         /* control range; 0 is loudest, 63 silent */
#define GAIN_LEVELS  20         /* what the USER moves in: 20 steps of 5% */
#define SPEAKER_CTL  "HpSpeaker Switch"   /* the only true mute on this codec */

/* Backlight. This device has no /sys/class/backlight; the panel is driven by
 * the Allwinner disp2 engine, and the firmware's own settings library goes
 * through /dev/disp. These are plain command numbers with an unsigned long[4]
 * argument block rather than _IOWR-encoded requests, so there is no struct size
 * to get wrong - validated against the running kernel by tools/dispprobe.c
 * before it was written here.
 *
 * Not inverted, unlike the mixer: 0 is dark, BRIGHT_RAW_MAX is full. */
#define DISP_LCD_SET_BRIGHTNESS 0x102
#define DISP_LCD_GET_BRIGHTNESS 0x103
#define BRIGHT_RAW_MAX 255
#define BRIGHT_RAW_MIN 8        /* never fully dark - a black screen looks broken */

static int g_disp_fd = -1;
static int g_bright = -1;       /* 0..GAIN_LEVELS, shares the level scale */

static int g_mixer_fd = -1;
static int g_level = -1;        /* 0..GAIN_LEVELS, or -1 before first read */
static uint64_t g_osd_until;    /* show the bar until this time */
static int      g_osd_level;    /* what the bar shows - volume OR brightness */

/* The port thinks in percent and converts; the inverted register never leaves
 * this file. Rounded both ways so a read-back lands on the level it came from. */
static int level_to_raw(int lv)
{
	return GAIN_RAW_MAX - (lv * GAIN_RAW_MAX + GAIN_LEVELS / 2) / GAIN_LEVELS;
}
static int raw_to_level(int raw)
{
	return ((GAIN_RAW_MAX - raw) * GAIN_LEVELS + GAIN_RAW_MAX / 2) / GAIN_RAW_MAX;
}

static int ctl_io(const char *name, long *val, int write)
{
	struct dm_ctl_elem_value v;

	if (g_mixer_fd < 0) return -1;
	memset(&v, 0, sizeof v);
	v.id.iface = 2;                                 /* SNDRV_CTL_ELEM_IFACE_MIXER */
	snprintf((char *)v.id.name, sizeof v.id.name, "%s", name);
	if (write) {
		v.value.integer.value[0] = *val;
		return ioctl(g_mixer_fd, DM_CTL_ELEM_WRITE, &v);
	}
	if (ioctl(g_mixer_fd, DM_CTL_ELEM_READ, &v) < 0) return -1;
	*val = v.value.integer.value[0];
	return 0;
}

static int gain_io(long *val, int write) { return ctl_io(GAIN_CTL, val, write); }

/* `dir` is +1 for louder. One step is 5% of the range. */
static void gain_nudge(int dir)
{
	long v;

	if (g_level < 0) {
		if (gain_io(&v, 0) < 0) return;
		g_level = raw_to_level((int)v);
	}
	g_level += dir;
	if (g_level < 0)           g_level = 0;
	if (g_level > GAIN_LEVELS) g_level = GAIN_LEVELS;
	v = level_to_raw(g_level);
	if (gain_io(&v, 1) < 0) return;

	/* Zero has to cut the path, not just attenuate it. The control advertises
	 * `mute=0`, meaning its minimum is maximum attenuation - about -74 dB -
	 * and not silence. Measured: with an ear against the speaker, level 0 is
	 * still audible, and a mic across the room cannot tell it from the room.
	 * So the speaker switch carries the last step. */
	v = (g_level > 0);
	ctl_io(SPEAKER_CTL, &v, 1);

	/* Feedback lives here too: the launcher owns UI, but it is not drawing
	 * while a game runs, so nothing else can show this. 1.5s from the last
	 * press, so holding a key keeps the bar up. */
	g_osd_level = g_level;
	g_osd_until = diatom_port_now_us() + 1500000ull;
}

static void bright_nudge(int dir)
{
	unsigned long a[4] = { 0, 0, 0, 0 };
	int raw;

	if (g_disp_fd < 0) return;
	if (g_bright < 0) {
		raw = ioctl(g_disp_fd, DISP_LCD_GET_BRIGHTNESS, a);
		if (raw < 0) return;
		g_bright = (raw * GAIN_LEVELS + BRIGHT_RAW_MAX / 2) / BRIGHT_RAW_MAX;
	}
	g_bright += dir;
	if (g_bright < 0)           g_bright = 0;
	if (g_bright > GAIN_LEVELS) g_bright = GAIN_LEVELS;

	raw = (g_bright * BRIGHT_RAW_MAX + GAIN_LEVELS / 2) / GAIN_LEVELS;
	if (raw < BRIGHT_RAW_MIN) raw = BRIGHT_RAW_MIN;
	a[1] = (unsigned long)raw;
	if (ioctl(g_disp_fd, DISP_LCD_SET_BRIGHTNESS, a) < 0) return;

	g_osd_level = g_bright;
	g_osd_until = diatom_port_now_us() + 1500000ull;
}

/* The one indicator, matching what the device UI draws so volume reads the same
 * in a game as on the shelf: a scrim across the very top with a 6px bar in it.
 * No glyph, no number - you know which button you just pressed.
 *
 * Drawn straight into the page after the blit and before publish, so it rides
 * the same flip and costs one pass over 12 rows. */
static void draw_gain_bar(uint8_t *base)
{
	const unsigned ro = g_vinfo.red.offset, go = g_vinfo.green.offset;
	const unsigned bo = g_vinfo.blue.offset;
	const int pad = 3, bar = 6;
	const int W = (int)g_vinfo.xres;
	int fill = g_osd_level < 0 ? 0 : (W * g_osd_level) / GAIN_LEVELS;
	int y, x;

	for (y = 0; y < pad * 2 + bar; y++) {
		uint32_t *row = (uint32_t *)(base + (size_t)y * g_finfo.line_length);
		for (x = 0; x < W; x++) {
			uint32_t p = row[x];
			if (y < pad || y >= pad + bar) {
				/* Scrim: halve what is already there. */
				uint32_t r = ((p >> ro) & 0xff) >> 1;
				uint32_t g = ((p >> go) & 0xff) >> 1;
				uint32_t b = ((p >> bo) & 0xff) >> 1;
				row[x] = (r << ro) | (g << go) | (b << bo) | g_opaque;
			} else if (x < fill) {
				row[x] = (235u << ro) | (235u << go) | (240u << bo) | g_opaque;
			} else {
				row[x] = (60u << ro) | (62u << go) | (72u << bo) | g_opaque;
			}
		}
	}
}

static uint8_t *page_base(int page)
{
	return g_fb + (size_t)page * g_vinfo.yres * g_finfo.line_length;
}

static void clear_pages(void);

static void *flip_worker(void *arg)
{
	struct fb_var_screeninfo v = g_vinfo;

	pthread_mutex_lock(&g_flip_mx);
	while (!g_flip_stop) {
		int page;

		if (g_pending < 0) {
			pthread_cond_wait(&g_flip_cv, &g_flip_mx);
			continue;
		}
		page = g_pending;
		g_pending  = -1;
		g_inflight = page;
		pthread_mutex_unlock(&g_flip_mx);

		/* Blocks until the address latches at a vsync - the whole reason
		 * this thread exists. */
		v.yoffset  = (uint32_t)page * v.yres;
		v.activate = FB_ACTIVATE_VBL;
		if (ioctl(g_fb_fd, FBIOPAN_DISPLAY, &v) != 0)
			diatom_port_log(DIATOM_LOG_WARN, "pan failed in flip thread");

		pthread_mutex_lock(&g_flip_mx);
		g_front    = page;
		g_inflight = -1;
		pthread_cond_broadcast(&g_flip_idle);
	}
	pthread_mutex_unlock(&g_flip_mx);
	return arg;
}

bool diatom_port_init(diatom_port_caps *out)
{
	SDL_AudioSpec want, have;

	g_mixer_fd = open("/dev/snd/controlC0", O_RDWR);
	g_disp_fd  = open("/dev/disp", O_RDWR);
	g_input_debug   = getenv("DIATOM_INPUT_DEBUG") != NULL;
	g_present_debug = getenv("DIATOM_PRESENT_DEBUG") != NULL;

	/* No SDL_INIT_VIDEO: presentation does not go through SDL at all, and the
	 * mali video driver would otherwise claim the display. */
	if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK) != 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return false;
	}

	g_fb_fd = open("/dev/fb0", O_RDWR);
	if (g_fb_fd < 0) { perror("open /dev/fb0"); return false; }
	if (ioctl(g_fb_fd, FBIOGET_FSCREENINFO, &g_finfo) != 0 ||
	    ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &g_vinfo) != 0) {
		perror("FBIOGET_SCREENINFO");
		return false;
	}
	if (g_vinfo.bits_per_pixel != 32) {
		/* The panel runs 32bpp (measured); anything else means the display
		 * setup changed and this port needs to learn the new format, not
		 * guess at it. */
		fprintf(stderr, "fb0 is %ubpp, port expects 32\n", g_vinfo.bits_per_pixel);
		return false;
	}

	g_pages = (int)(g_finfo.smem_len / ((size_t)g_vinfo.yres * g_finfo.line_length));
	if (g_pages > FB_PAGES) g_pages = FB_PAGES;
	if (g_pages < 1) { fprintf(stderr, "fb0 too small for one page\n"); return false; }

	g_fb_size = (size_t)g_pages * g_vinfo.yres * g_finfo.line_length;
	g_fb = mmap(NULL, g_fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);
	if (g_fb == MAP_FAILED) { perror("mmap fb0"); g_fb = NULL; return false; }

	if (g_vinfo.transp.length > 0)
		g_opaque = ((1u << g_vinfo.transp.length) - 1) << g_vinfo.transp.offset;

	/* Opaque black, not memset zero: zero alpha is invisible, see g_opaque. */
	clear_pages();

	/* Start from a known page. The mailbox needs three pages - front, in
	 * flight, drawing; fewer, or a refused pan, means single-buffered
	 * drawing straight to glass: can tear, still works. */
	g_vinfo.yoffset = 0;
	if (ioctl(g_fb_fd, FBIOPAN_DISPLAY, &g_vinfo) != 0 || g_pages < 3) {
		g_pan_broken = true;
		diatom_port_log(DIATOM_LOG_WARN, "fb0 pan or pages unavailable; single-buffered");
	} else if (pthread_create(&g_flip_thread, NULL, flip_worker, NULL) != 0) {
		g_pan_broken = true;
		diatom_port_log(DIATOM_LOG_WARN, "no flip thread; single-buffered");
	} else {
		g_flip_running = true;
	}
	g_front = 0;

	SDL_memset(&want, 0, sizeof want);
	want.freq     = AUDIO_RATE;
	want.format   = AUDIO_S16SYS;
	want.channels = 2;
	want.samples  = 1024;
	g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (!g_audio) { fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError()); return false; }
	SDL_PauseAudioDevice(g_audio, 0);

	/* The Brick's buttons arrive as one joystick (kernel name "TRIMUI
	 * Player1"; the firmware's SDL reports it as "Xbox 360 Controller").
	 * The keyboard-class devices carry only volume and power keys - measured
	 * from the kernel capability bitmasks, see the mapping tables below. */
	if (SDL_NumJoysticks() > 0)
		g_joy = SDL_JoystickOpen(0);
	if (!g_joy)
		diatom_port_log(DIATOM_LOG_WARN, "no joystick found; no game input");

	{
		char msg[160];
		snprintf(msg, sizeof msg,
		         "brick: fb %ux%u stride %u, %d page(s), rgba at %u/%u/%u/%u+%u, audio %d Hz, joystick %s",
		         g_vinfo.xres, g_vinfo.yres, g_finfo.line_length, g_pages,
		         g_vinfo.red.offset, g_vinfo.green.offset, g_vinfo.blue.offset,
		         g_vinfo.transp.offset, g_vinfo.transp.length,
		         have.freq, g_joy ? SDL_JoystickName(g_joy) : "none");
		diatom_port_log(DIATOM_LOG_INFO, msg);
	}

	out->surface_w           = (int)g_vinfo.xres;
	out->surface_h           = (int)g_vinfo.yres;
	out->audio_rate          = have.freq;
	out->audio_buffer_frames = AUDIO_BUFFER_FRAMES;
	out->present_blocks      = false;
	return true;
}

void diatom_port_shutdown(void)
{
	if (g_flip_running) {
		pthread_mutex_lock(&g_flip_mx);
		g_flip_stop = true;
		pthread_cond_signal(&g_flip_cv);
		pthread_mutex_unlock(&g_flip_mx);
		pthread_join(g_flip_thread, NULL);
	}
	free(g_colmap);
	free(g_rowmap);
	/* Always hand the speaker back on, whatever the level was.
	 *
	 * Muting at level 0 switches HpSpeaker off, and that is device state which
	 * outlives this process. Leaving it off strands the device: the launcher
	 * drives `digital volume`, NOT this switch, so turning the volume up there
	 * cannot undo it and the machine simply appears to have lost its speaker.
	 *
	 * The volume LEVEL is deliberately not restored - that is a user setting
	 * and belongs wherever they left it. The switch is a mechanism, and nothing
	 * above this port knows it exists. */
	if (g_mixer_fd >= 0) {
		long on = 1;
		ctl_io(SPEAKER_CTL, &on, 1);
		close(g_mixer_fd);
		g_mixer_fd = -1;
	}
	/* Brightness is NOT restored: it is a user setting and the launcher
	 * re-applies its own on resume anyway. Unlike the speaker switch, leaving
	 * it strands nothing - the launcher's own control can always move it. */
	if (g_disp_fd >= 0) { close(g_disp_fd); g_disp_fd = -1; }
	if (g_fb)         munmap(g_fb, g_fb_size);
	if (g_fb_fd >= 0) close(g_fb_fd);
	if (g_joy)        SDL_JoystickClose(g_joy);
	if (g_audio)      SDL_CloseAudioDevice(g_audio);
	SDL_Quit();
}

/* One axis of the resampling map.
 *
 * Sharp-bilinear: take the bilinear weight, then steepen the ramp by the scale
 * factor so the blend spans one DESTINATION pixel rather than one source
 * pixel. Interior pixels come out exact and only the pixel straddling a source
 * boundary is mixed, which is why it looks like integer scaling with the
 * unevenness taken out rather than like a blur.
 *
 * At an integer factor the steepened weight lands on 0 or 1 for every pixel,
 * so the output is identical to nearest by construction - the `integer-sharp`
 * preset exists to demonstrate exactly that.
 *
 * Doubles here are free: this runs once per geometry or filter change, and the
 * per-pixel work uses only the integers it produces. */
static diatom_tap *build_map(int src, int dst, diatom_filter filter)
{
	diatom_tap *m;
	double scale;
	int i;

	if (src <= 0 || dst <= 0) return NULL;
	m = malloc((size_t)dst * sizeof *m);
	if (!m) return NULL;
	scale = (double)dst / (double)src;

	for (i = 0; i < dst; i++) {
		double c  = ((double)i + 0.5) / scale - 0.5;   /* source coordinate */
		int    p  = (int)floor(c);
		double fr = c - (double)p;

		fr = (fr - 0.5) * scale + 0.5;
		if (fr < 0.0) fr = 0.0;
		if (fr > 1.0) fr = 1.0;

		/* Clamp at the edges: no pixel outside the source to blend towards. */
		if (p < 0)        { p = 0;       fr = 0.0; }
		if (p >= src - 1) { p = src - 1; fr = 0.0; }

		if (filter == DIATOM_FILTER_NEAREST) {
			if (fr >= 0.5 && p + 1 < src) p++;
			fr = 0.0;
		}

		m[i].idx = p;
		m[i].w   = (int)(fr * 256.0 + 0.5);
		if (m[i].w > 256) m[i].w = 256;
	}

	/* Collapse a map whose every weight is 0 or 256 onto the fast path. At a
	 * whole factor sharp-bilinear is a no-op by construction, and this makes it
	 * a no-op in cost too: measured, sharp on an integer rect was paying 2.4 ms
	 * a frame to compute the same pixels. */
	for (i = 0; i < dst; i++)
		if (m[i].w != 0 && m[i].w != 256) return m;
	for (i = 0; i < dst; i++)
		if (m[i].w == 256) { m[i].idx++; m[i].w = 0; }
	return m;
}

static bool ensure_maps(int src_w, int src_h, diatom_rect dst,
                        diatom_filter filter)
{
	if (g_map_valid && src_w == g_map_src_w && src_h == g_map_src_h &&
	    filter == g_map_filter && !memcmp(&dst, &g_map_dst, sizeof dst))
		return true;

	free(g_colmap);
	free(g_rowmap);
	g_colmap = build_map(src_w, dst.w, filter);
	g_rowmap = build_map(src_h, dst.h, filter);
	g_map_valid = g_colmap && g_rowmap;
	if (!g_map_valid) return false;

	g_map_src_w = src_w;
	g_map_src_h = src_h;
	g_map_dst   = dst;
	g_map_filter = filter;
	return true;
}

static inline diatom_rgb un565(uint16_t c)
{
	diatom_rgb v;
	int r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
	v.r = (int)((r << 3) | (r >> 2));
	v.g = (int)((g << 2) | (g >> 4));
	v.b = (int)((b << 3) | (b >> 2));
	return v;
}

static inline diatom_rgb un8888(uint32_t c)
{
	diatom_rgb v;
	v.r = (int)((c >> 16) & 0xff);
	v.g = (int)((c >>  8) & 0xff);
	v.b = (int)( c        & 0xff);
	return v;
}

/* Converted source rows, so a source pixel is converted ONCE rather than once
 * per destination pixel that samples it.
 *
 * Measured 2026-08-25: at 256x224 into 1024x768 the naive loop costs 7.52 ms
 * and this costs 3.37 ms, a 2.23x saving, because 4x horizontal and 3.4x
 * vertical scaling meant roughly thirteen redundant conversions of every source
 * pixel. The same measurement showed framebuffer memory is exactly as fast as
 * ordinary heap - 418 MB/s either way - so the cost was never the write, and
 * the disp2 hardware scaler would have been solving the wrong problem.
 *
 * Two slots because a blended row needs the source row above and below. Rows
 * are looked up by index, so consecutive destination rows sampling the same
 * source row convert nothing at all. */
#define ROWCACHE_MAX 2048     /* widest source in the matrix is 512 (SNES hires) */
static uint32_t g_rc[2][ROWCACHE_MAX];
static int      g_rc_row[2] = { -1, -1 };

static const uint32_t *cache_row(const void *src, size_t pitch, diatom_pixfmt fmt,
                                 int row, int src_w)
{
	const unsigned ro = g_vinfo.red.offset;
	const unsigned go = g_vinfo.green.offset;
	const unsigned bo = g_vinfo.blue.offset;
	const uint32_t opaque = g_opaque;
	uint32_t *dst;
	int slot, x;

	if (g_rc_row[0] == row) return g_rc[0];
	if (g_rc_row[1] == row) return g_rc[1];

	/* Evict the older row. Destination rows advance monotonically, so the
	 * lower index is the one that will not be wanted again. */
	slot = (g_rc_row[0] <= g_rc_row[1]) ? 0 : 1;
	dst  = g_rc[slot];

	if (fmt == DIATOM_PIX_RGB565) {
		const uint16_t *in = (const uint16_t *)((const uint8_t *)src + (size_t)row * pitch);
		for (x = 0; x < src_w; x++) {
			const uint16_t c = in[x];
			const uint32_t r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
			dst[x] = opaque
			       | (((r << 3) | (r >> 2)) << ro)
			       | (((g << 2) | (g >> 4)) << go)
			       | (((b << 3) | (b >> 2)) << bo);
		}
	} else {
		const uint32_t *in = (const uint32_t *)((const uint8_t *)src + (size_t)row * pitch);
		for (x = 0; x < src_w; x++) {
			const uint32_t c = in[x];
			dst[x] = opaque
			       | (((c >> 16) & 0xff) << ro)
			       | (((c >>  8) & 0xff) << go)
			       | (( c        & 0xff) << bo);
		}
	}
	g_rc_row[slot] = row;
	return dst;
}

static inline diatom_rgb unpack(uint32_t p)
{
	diatom_rgb v;
	v.r = (int)((p >> g_vinfo.red.offset)   & 0xff);
	v.g = (int)((p >> g_vinfo.green.offset) & 0xff);
	v.b = (int)((p >> g_vinfo.blue.offset)  & 0xff);
	return v;
}

static inline diatom_rgb mix(diatom_rgb a, diatom_rgb b, int w)
{
	diatom_rgb v;
	v.r = a.r + (((b.r - a.r) * w) >> 8);
	v.g = a.g + (((b.g - a.g) * w) >> 8);
	v.b = a.b + (((b.b - a.b) * w) >> 8);
	return v;
}

/* Scale-blit src into the destination rect of one page, converting to the
 * framebuffer's own channel order (offsets read from the driver, not assumed).
 *
 * CLIPS rather than refuses: a fill or overscale mode hands over a rect larger
 * than the panel on purpose, and dropping the frame would be the wrong answer
 * to a deliberate crop. */
static void blit(uint8_t *page, const void *src, int w, int h, size_t pitch,
                 diatom_pixfmt fmt, diatom_rect dst)
{
	const unsigned ro = g_vinfo.red.offset;
	const unsigned go = g_vinfo.green.offset;
	const unsigned bo = g_vinfo.blue.offset;
	const uint32_t opaque = g_opaque;
	int x0, x1, y0, y1, y;

#define PACK(c) (opaque | ((uint32_t)(c).r << ro) \
                        | ((uint32_t)(c).g << go) \
                        | ((uint32_t)(c).b << bo))

/* Two loops, not one with a branch: the vertical weight is constant across a
 * row, so testing it per pixel would be 1024 redundant branches per row.
 *
 * Both source from the converted row cache, so the nearest path - the default,
 * and every pixel at a whole factor - is a pure indexed copy with no arithmetic
 * at all. */
#define BLIT_ROW_NEAREST() do {                                              \
	int x;                                                                   \
	for (x = x0; x < x1; x++) {                                              \
		diatom_tap tx = g_colmap[x - dst.x];                                 \
		if (!tx.w) out[x] = c0[tx.idx];                                      \
		else       out[x] = PACK(mix(unpack(c0[tx.idx]),                     \
		                             unpack(c0[tx.idx + 1]), tx.w));         \
	}                                                                        \
} while (0)

#define BLIT_ROW_BLEND() do {                                                \
	int x;                                                                   \
	for (x = x0; x < x1; x++) {                                              \
		diatom_tap tx = g_colmap[x - dst.x];                                 \
		diatom_rgb a = unpack(c0[tx.idx]);                                   \
		diatom_rgb b = unpack(c1[tx.idx]);                                   \
		if (tx.w) {                                                          \
			a = mix(a, unpack(c0[tx.idx + 1]), tx.w);                        \
			b = mix(b, unpack(c1[tx.idx + 1]), tx.w);                        \
		}                                                                    \
		out[x] = PACK(mix(a, b, ty.w));                                      \
	}                                                                        \
} while (0)

	if (!g_map_valid || w > ROWCACHE_MAX) return;

	y0 = dst.y > 0 ? dst.y : 0;
	x0 = dst.x > 0 ? dst.x : 0;
	y1 = dst.y + dst.h < (int)g_vinfo.yres ? dst.y + dst.h : (int)g_vinfo.yres;
	x1 = dst.x + dst.w < (int)g_vinfo.xres ? dst.x + dst.w : (int)g_vinfo.xres;
	if (x1 <= x0 || y1 <= y0) return;

	/* The cache is per-frame: the source buffer is rewritten every time. */
	g_rc_row[0] = g_rc_row[1] = -1;

	for (y = y0; y < y1; y++) {
		diatom_tap ty = g_rowmap[y - dst.y];
		uint32_t *out = (uint32_t *)(page + (size_t)y * g_finfo.line_length);
		const uint32_t *c0 = cache_row(src, pitch, fmt, ty.idx, w);
		const uint32_t *c1 = ty.w ? cache_row(src, pitch, fmt, ty.idx + 1, w) : NULL;

		if (ty.w) BLIT_ROW_BLEND();
		else      BLIT_ROW_NEAREST();
	}

	/* Letterbox bars are not repainted per frame: pages start opaque black and
	 * the rect only shrinks when the user changes mode, which clears them. */
#undef BLIT_ROW_NEAREST
#undef BLIT_ROW_BLEND
#undef PACK
}

/* Wipe every page to opaque black. Needed when the rect shrinks, or the
 * previous mode's picture stays framing the new one. */
static void clear_pages(void)
{
	uint32_t *p = (uint32_t *)g_fb;
	size_t n = g_fb_size / sizeof *p, i;
	for (i = 0; i < n; i++) p[i] = g_opaque;
}

void diatom_port_present(const void *src, int w, int h, size_t pitch,
                         diatom_pixfmt fmt, diatom_rect dst,
                         diatom_filter filter)
{
	uint64_t t0 = 0, t1 = 0;
	bool rect_changed;
	int page;

	/* Dupe frame: the front page already shows it. Nothing to draw, nothing
	 * to flip. */
	if (!src || w <= 0 || h <= 0) return;

	rect_changed = !g_map_valid || memcmp(&dst, &g_map_dst, sizeof dst) != 0;
	if (!ensure_maps(w, h, dst, filter)) return;
	/* A smaller rect leaves the old picture around the new one. Only on a mode
	 * change, so the cost of wiping every page does not matter. */
	if (rect_changed) clear_pages();

	if (g_pan_broken) {
		blit(page_base(g_front), src, w, h, pitch, fmt, dst);
		if (diatom_port_now_us() < g_osd_until) draw_gain_bar(page_base(g_front));
		return;
	}

	/* Pick a page holding no role. If every page is spoken for - the panel is
	 * consuming slower than the core produces - steal the pending one: the
	 * thread has not started panning it, so overwriting it just replaces a
	 * frame nobody saw with a newer one. Latest wins. */
	pthread_mutex_lock(&g_flip_mx);
	for (page = 0; page < g_pages; page++)
		if (page != g_front && page != g_inflight && page != g_pending)
			break;
	if (page == g_pages) {
		page      = g_pending;
		g_pending = -1;
	}
	pthread_mutex_unlock(&g_flip_mx);

	if (g_present_debug) t0 = diatom_port_now_us();

	blit(page_base(page), src, w, h, pitch, fmt, dst);
	if (diatom_port_now_us() < g_osd_until) draw_gain_bar(page_base(page));

	pthread_mutex_lock(&g_flip_mx);
	g_pending = page;
	pthread_cond_signal(&g_flip_cv);
	pthread_mutex_unlock(&g_flip_mx);

	if (g_present_debug) {
		static int n;
		t1 = diatom_port_now_us();
		if (++n >= 60) {
			char msg[96];
			n = 0;
			snprintf(msg, sizeof msg, "present: blit+publish %llu us",
			         (unsigned long long)(t1 - t0));
			diatom_port_log(DIATOM_LOG_DEBUG, msg);
		}
	}
}

size_t diatom_port_audio_write(const int16_t *frames, size_t n)
{
	size_t queued, room;

	/* Never blocks; drops on overflow. A blocking write would pace the whole
	 * program off the audio clock, which rules out dynamic rate control.
	 *
	 * Take only what fits. The previous test was `queued >= capacity`, which
	 * refuses only an ALREADY FULL queue and then admits a whole batch: at 4095
	 * queued a 832-frame batch landed entire, and the queue reached a measured
	 * **4927 against a stated 4096**. That mattered beyond the extra latency,
	 * because rate control clamps its error term at +1.0 - reached at exactly
	 * capacity - so everything above capacity was invisible to the controller
	 * that was supposed to be preventing it.
	 *
	 * Clamping rather than refusing the batch outright loses less: the frames
	 * that do not fit are dropped either way, and there is no reason to discard
	 * the ones that would have. */
	if (!g_audio || !frames || !n) return 0;
	queued = diatom_port_audio_queued();
	room   = queued >= (size_t)AUDIO_BUFFER_FRAMES
	       ? 0 : (size_t)AUDIO_BUFFER_FRAMES - queued;
	if (n > room) n = room;
	if (!n) return 0;

	SDL_QueueAudio(g_audio, frames, (Uint32)(n * AUDIO_FRAME_BYTES));
	return n;
}

size_t diatom_port_audio_queued(void)
{
	if (!g_audio) return 0;
	return SDL_GetQueuedAudioSize(g_audio) / AUDIO_FRAME_BYTES;
}

/* SDL joystick button index -> Diatom button.
 *
 * The index side is DERIVED: the kernel capability bitmask for "TRIMUI
 * Player1" (/proc/bus/input/devices, B: KEY=) decodes to exactly these
 * codes, and SDL's Linux joystick driver assigns indices in ascending code
 * order, gamepad range before low keycodes:
 *
 *   0  304 BTN_SOUTH    4  310 BTN_TL      8  316 BTN_MODE    12  60 KEY_F2
 *   1  305 BTN_EAST     5  311 BTN_TR      9  317 BTN_THUMBL  13 114 VOL_DN
 *   2  307 BTN_NORTH    6  314 BTN_SELECT 10  318 BTN_THUMBR  14 115 VOL_UP
 *   3  308 BTN_WEST     7  315 BTN_START  11   59 KEY_F1
 *
 * The label side is MEASURED: every cap pressed in a known order under
 * DIATOM_INPUT_DEBUG, twice, 2026-08-24, firmware 1.1.1. A and B follow the
 * positional reading (EAST = right cap = A, SOUTH = bottom = B), but X and Y
 * are the other way around from position: the top cap (X) emits BTN_WEST and
 * the left cap (Y) emits BTN_NORTH. Positional reasoning got exactly those
 * two wrong, which is why this table is measured and not argued.
 *
 * The dpad is ABS_HAT0 - hat values are semantic, no attribution needed.
 * THUMBL/THUMBR/F1/F2 exist in the mask because the same driver serves
 * stick-bearing siblings; volume is the host OS's business. All unmapped. */
static const struct { int idx; int btn; } joymap[] = {
	{ 0, DIATOM_BTN_B },      { 1, DIATOM_BTN_A },
	{ 2, DIATOM_BTN_Y },      { 3, DIATOM_BTN_X },
	{ 4, DIATOM_BTN_L1 },     { 5, DIATOM_BTN_R1 },
	{ 6, DIATOM_BTN_SELECT }, { 7, DIATOM_BTN_START },
	{ 8, DIATOM_BTN_MENU },
};

/* L2/R2 are digital switches surfaced as axes (the KEY mask has no
 * BTN_TL2/TR2, the ABS mask advertises X Y Z RX RY RZ). Measured 2026-08-24:
 * L2 is SDL axis 2 (ABS_Z), R2 is axis 5 (ABS_RZ), resting at -32768 and
 * slamming to +32767 when pressed - a digital switch in axis clothing, so
 * half travel is a comfortable threshold. The other four axes belong to the
 * stick-bearing siblings this driver also serves. */
/* Volume, from the same measured table: SDL indices 13 and 14 are VOL_DN and
 * VOL_UP. Absent from joymap[] on purpose - they are not game inputs. */
#define JOY_VOL_DN 13
#define JOY_VOL_UP 14

/* The two front keys, reported as BTN_THUMBL/THUMBR - SDL indices 9 and 10,
 * which minarch calls L3/R3. They are not game inputs; the firmware spends
 * them on brightness and so do we. Absent from joymap[] on purpose: mapping
 * them would send every brightness press to the core. */
#define JOY_FN_L   9
#define JOY_FN_R   10

#define AXIS_L2 2
#define AXIS_R2 5
#define AXIS_PRESSED 16384

static void debug_event(const char *what, int a, int b)
{
	char msg[96];
	if (!g_input_debug) return;
	snprintf(msg, sizeof msg, "%s %d -> %d", what, a, b);
	diatom_port_log(DIATOM_LOG_DEBUG, msg);
}

void diatom_port_input_poll(void)
{
	SDL_Event ev;
	size_t i;

	while (SDL_PollEvent(&ev)) {
		switch (ev.type) {
		case SDL_QUIT:
			g_quit = true;
			break;

		case SDL_JOYBUTTONDOWN:
		case SDL_JOYBUTTONUP: {
			bool down = (ev.type == SDL_JOYBUTTONDOWN);
			debug_event("joy button", ev.jbutton.button, down);

			/* Volume is the port's, and stops here. Whoever owns the
			 * input loop during a game has to handle these, because
			 * nothing else sees them - the device UI is not running.
			 * They are never reported upward and never reach a core. */
			if (ev.jbutton.button == JOY_VOL_UP) {
				if (down) gain_nudge(+1);
				break;
			}
			if (ev.jbutton.button == JOY_VOL_DN) {
				if (down) gain_nudge(-1);
				break;
			}
			if (ev.jbutton.button == JOY_FN_R) {
				if (down) bright_nudge(+1);
				break;
			}
			if (ev.jbutton.button == JOY_FN_L) {
				if (down) bright_nudge(-1);
				break;
			}

			for (i = 0; i < sizeof joymap / sizeof joymap[0]; i++) {
				if (joymap[i].idx != ev.jbutton.button) continue;
				if (down) g_buttons |=  DIATOM_BIT(joymap[i].btn);
				else      g_buttons &= ~DIATOM_BIT(joymap[i].btn);
			}
			break;
		}

		case SDL_JOYHATMOTION: {
			uint32_t dpad = 0;
			debug_event("joy hat", ev.jhat.hat, ev.jhat.value);
			if (ev.jhat.value & SDL_HAT_UP)    dpad |= DIATOM_BIT(DIATOM_BTN_UP);
			if (ev.jhat.value & SDL_HAT_DOWN)  dpad |= DIATOM_BIT(DIATOM_BTN_DOWN);
			if (ev.jhat.value & SDL_HAT_LEFT)  dpad |= DIATOM_BIT(DIATOM_BTN_LEFT);
			if (ev.jhat.value & SDL_HAT_RIGHT) dpad |= DIATOM_BIT(DIATOM_BTN_RIGHT);
			g_buttons = (g_buttons
			             & ~(DIATOM_BIT(DIATOM_BTN_UP)   | DIATOM_BIT(DIATOM_BTN_DOWN)
			               | DIATOM_BIT(DIATOM_BTN_LEFT) | DIATOM_BIT(DIATOM_BTN_RIGHT)))
			            | dpad;
			break;
		}

		case SDL_JOYAXISMOTION: {
			bool pressed = ev.jaxis.value > AXIS_PRESSED;
			debug_event("joy axis", ev.jaxis.axis, ev.jaxis.value);
			if (ev.jaxis.axis == AXIS_L2) {
				if (pressed) g_buttons |=  DIATOM_BIT(DIATOM_BTN_L2);
				else         g_buttons &= ~DIATOM_BIT(DIATOM_BTN_L2);
			} else if (ev.jaxis.axis == AXIS_R2) {
				if (pressed) g_buttons |=  DIATOM_BIT(DIATOM_BTN_R2);
				else         g_buttons &= ~DIATOM_BIT(DIATOM_BTN_R2);
			}
			break;
		}
		}
	}

}

uint32_t diatom_port_input_state(void) { return g_buttons; }
bool     diatom_port_should_quit(void) { return g_quit; }

bool diatom_port_capture(const char *path)
{
	SDL_Surface *s, *rgb;
	bool ok;

	int front;

	if (!g_fb || !path) return false;

	/* Wait for the mailbox to drain before reading the front page.
	 *
	 * Without this, a capture reads whichever page the flip thread last got to,
	 * which depends on how long the blit took - so the same run captured at the
	 * same frame count could yield the frame before. Found 2026-08-25 while
	 * checking a blit optimisation for correctness: old and new binaries
	 * produced captures differing in one 24-row band, and only at some frame
	 * counts, which is a blinking sprite one frame apart rather than a blit
	 * defect. An instrument that is not deterministic cannot verify anything. */
	pthread_mutex_lock(&g_flip_mx);
	while (g_flip_running && (g_pending >= 0 || g_inflight >= 0))
		pthread_cond_wait(&g_flip_idle, &g_flip_mx);
	front = g_front;
	pthread_mutex_unlock(&g_flip_mx);

	/* Read back the page on glass. Masks come from the driver's reported
	 * channel offsets, same as the blit writes. */
	s = SDL_CreateRGBSurfaceFrom(page_base(front),
	                             (int)g_vinfo.xres, (int)g_vinfo.yres, 32,
	                             (int)g_finfo.line_length,
	                             0xffu << g_vinfo.red.offset,
	                             0xffu << g_vinfo.green.offset,
	                             0xffu << g_vinfo.blue.offset, 0);
	if (!s) return false;

	/* Plain 24-bit RGB: a 32-bit BMP carries a V4/V5 header several readers
	 * refuse, and the alpha channel is meaningless here anyway. */
	rgb = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_RGB24, 0);
	SDL_FreeSurface(s);
	if (!rgb) return false;
	ok = SDL_SaveBMP(rgb, path) == 0;
	SDL_FreeSurface(rgb);
	return ok;
}

uint64_t diatom_port_now_us(void)
{
	return (uint64_t)(SDL_GetPerformanceCounter() * 1000000ULL
	                  / SDL_GetPerformanceFrequency());
}

void diatom_port_log(diatom_log_level lvl, const char *msg)
{
	static const char *tag[] = { "debug", "info", "warn", "error" };
	fprintf(stderr, "[%s] %s\n", tag[lvl], msg);
}
