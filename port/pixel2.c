/* GKD Pixel 2 port - ADR-0035 and ADR-0036.
 *
 * The Pixel 2 is an RK3326S with a Mali-G31 and a 640x480 panel that is
 * physically 480x640 portrait, mounted turned. Its system is TortOS-px2:
 * mainline Linux, Mesa's Panfrost, SDL2 with ALSA, no udev, no fbdev.
 *
 * Presentation is OpenGL ES 3 on a GBM surface, page-flipped through KMS by a
 * thread of its own. present() copies the core's frame into a latest-wins
 * mailbox and returns; the thread uploads it as a texture, draws it scaled and
 * turned in one quad, and flips at the panel's rate. That is ADR-0013's flip
 * thread with the blit moved to the GPU, because the CPU blit measured 7.7 ms
 * a frame here against a SNES core that already takes 10.5 (ADR-0035).
 *
 * The panel's orientation never leaves this file: the surface is reported as
 * 640x480, and the turn is in the quad's vertices (ADR-0007).
 *
 * The display is shared with the launcher by DRM master, held only between the
 * first present after a stop and the end of the next present_stop (ADR-0036).
 *
 * SDL2 provides audio and the clock only. Buttons are read straight from the
 * kernel's input devices, found by what they report.
 *
 * This file does not include libretro.h and must never need to.
 */
#include <SDL.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "diatom_port.h"
#include "port_clock.h"

#define AUDIO_RATE 48000
/* Capacity in FRAMES, the Brick's figure: 4096 at 48 kHz is about 85 ms, with
 * rate control holding it near half. Measured here 2026-10-01 through the
 * desktop port and the system's dmix: 1,200 frames of Contra, nothing dropped,
 * the queue between 131 and 2912. */
#define AUDIO_BUFFER_FRAMES 4096
#define AUDIO_FRAME_BYTES   (2 * (int)sizeof(int16_t))

/* ---------- the display ---------------------------------------------------- */

static int                 g_drm_fd = -1;
static struct gbm_device  *g_gbm;
static struct gbm_surface *g_gbm_surface;
static struct gbm_bo      *g_shown_bo;      /* on glass, released after the next flip */
static uint32_t            g_crtc_id, g_connector_id;
static int                 g_crtc_pipe;     /* index in the resources, for vblank waits */
static drmModeModeInfo     g_mode;
static bool                g_crtc_lit;      /* something already scans out on the CRTC */
static bool                g_have_master;

static EGLDisplay g_egl_display = EGL_NO_DISPLAY;
static EGLContext g_egl_context = EGL_NO_CONTEXT;
static EGLSurface g_egl_surface = EGL_NO_SURFACE;

/* The panel in its own terms, and the surface as the host sees it. On this
 * device the panel is 480x640 and the surface 640x480; g_turned says the two
 * differ. */
static int  g_panel_w, g_panel_h;
static int  g_surface_w, g_surface_h;
static bool g_turned;

/* Frames handed over by present(). Three, with the Brick's roles: front is
 * the last one drawn (kept for a capture), inflight is being drawn now,
 * pending is published and waiting. present() writes into whichever holds no
 * role and steals pending when all three are taken. Latest wins. */
#define MAILBOX_FRAMES 3
typedef struct {
	uint8_t      *pixels;
	size_t        cap;
	int           w, h;
	size_t        pitch;
	diatom_pixfmt fmt;
	diatom_rect   dst;
	diatom_filter filter;
} mailbox_frame;

static mailbox_frame    g_frames[MAILBOX_FRAMES];
static pthread_t        g_thread;
static pthread_mutex_t  g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_cv = PTHREAD_COND_INITIALIZER;     /* work for the thread */
static pthread_cond_t   g_idle = PTHREAD_COND_INITIALIZER;   /* the thread went idle */
static int              g_front = -1, g_inflight = -1, g_pending = -1;
static bool             g_thread_running, g_thread_stop;
static bool             g_want_blank;       /* present_stop(BLANK) asked for black */
static bool             g_blanking;
static const char      *g_capture_path;     /* a capture asked for, and its answer */
static bool             g_capture_ok;
static bool             g_presented;        /* anything presented since the last stop */

/* Startup handshake: the EGL context belongs to the thread, so the thread
 * builds it and init waits to hear whether that worked. */
static int              g_thread_ready;     /* 0 waiting, 1 ok, -1 failed */

static bool g_present_debug;

/* GL objects, all owned by the thread. */
static GLuint g_prog_plain, g_prog_sharp, g_prog_solid;
static GLuint g_frame_tex, g_overlay_tex;
static int    g_tex_w = -1, g_tex_h = -1;
static diatom_pixfmt g_tex_fmt;

/* ---------- the overlay and the level bar ----------------------------------- */

/* The overlay from diatom_port_overlay: a borrowed pointer, not a copy (see
 * diatom_port.h). The thread uploads it under g_mx, and the host's next call
 * takes g_mx first, so the buffer is never read after the host lets go of it. */
static const uint8_t *g_ov;
static int            g_ov_w, g_ov_h;
static uint64_t       g_ov_until;
static bool           g_ov_dirty;           /* a new image to upload */

static uint64_t g_osd_until;
static int      g_osd_level, g_osd_max, g_osd_kind;   /* kind 0 volume, 1 brightness */

/* ---------- levels ---------------------------------------------------------- */

/* Output gain, by ioctl on /dev/snd/controlC0, with the kernel's struct
 * vendored the way the Brick port does it (see port/brick.c for why a raw
 * ioctl and not alsa-lib). The RK817's "Master Playback Volume" is 0-255, two
 * channels, and not inverted: 255 is 0 dB, 0 is -95 dB (the driver's own
 * scale, read with amixer 2026-10-01). It has no switch; speaker or headphones
 * is "Playback Mux", which the system's jackswitch daemon drives, not us. */
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

#define GAIN_CTL     "Master Playback Volume"
#define GAIN_LEVELS  20         /* what the USER moves in: 20 steps of 5% */

/* The windows the twenty levels spread over, as ATTENUATION from the
 * register's top: 0 is 255, 0 dB.
 *
 * NOT YET MEASURED. These are a starting point to be set by ear with a game
 * playing, the way the Brick's were: 255 at the top, and a floor 45 dB down
 * (121 steps of the driver's 0.37 dB), the range the Brick's speaker ladder
 * spans. Headphones start on the same window and will want their own.
 *
 * Level 0 is raw 0, -95 dB, as the cut: the control has no switch. Whether
 * that is silent by ear is also unmeasured.
 *
 * Attenuation, not raw, because that is how TortOS's launcher holds it (its
 * src/device/pixel2.c): its shared ladder arithmetic was written for the
 * Brick's inverted register, loud end low, and attenuation is that shape. The
 * same numbers and the same rounding on both sides, for the reason brick.c
 * gives: a level that crosses the socket has to mean the same thing on both.
 * Change one, change the other. */
#define GAIN_RAW_MAX    255
#define SPK_ATT_TOP     0
#define SPK_ATT_BOTTOM  121
#define HP_ATT_TOP      0
#define HP_ATT_BOTTOM   121

static bool g_muted;                /* the launcher's, ADR-0031 */
static int  g_mixer_fd = -1;
static int  g_level = -1;           /* 0..GAIN_LEVELS, or -1 before first read */

/* Backlight, /sys/class/backlight/backlight, 0-255, 0 is off. The ladder is
 * the Brick's, which is the launcher's rungs: brightness is perceived in
 * ratios, so equal ratios read as equal steps (spike 2026-08-26). On this
 * panel 1 was still lit and 0 dark, checked by eye 2026-10-01; the floor of 2
 * is therefore visible here too, though not separately measured. */
#define BACKLIGHT "/sys/class/backlight/backlight/brightness"
static const unsigned char bright_ladder[] = {
	2, 4, 8, 16, 32, 48, 72, 96, 128, 160, 192, 255
};
#define BRIGHT_LEVELS ((int)(sizeof bright_ladder / sizeof bright_ladder[0]) - 1)
static int g_bright = -1;           /* index into bright_ladder, or -1 unread */

/* ---------- input ----------------------------------------------------------- */

#define BITS_PER_LONG   (8 * (int)sizeof(long))
#define NLONGS(max)     (((max) + BITS_PER_LONG) / BITS_PER_LONG)
#define BIT_IS_SET(a,b) (((a)[(b) / BITS_PER_LONG] >> ((b) % BITS_PER_LONG)) & 1UL)

static int      g_pad_fd = -1;      /* the gamepad: "gkd_pixel2_joypad" */
static int      g_keys_fd = -1;     /* the volume keys: "gpio-keys" */
static int      g_jack_fd = -1;     /* the codec's jack switch */
static uint32_t g_buttons;
static bool     g_input_debug;

/* Kernel code -> Diatom button.
 *
 * The codes are the device tree's (ROCKNIX's rk3326s-gkd-pixel2.dts), and the
 * labels are MEASURED: every key pressed in a known order on 2026-10-01 with a
 * logger reading the kernel's events. The caps marked A, B, X, Y send SOUTH,
 * EAST, NORTH, WEST - not the Brick's arrangement, where X and Y were the
 * other way round from position. The codes are named for the labels, not for
 * where the caps sit: A is on the right and B at the bottom, so the A cap
 * sends SOUTH. Mapping by label is what puts the right-hand button on the
 * game's A, as on a SNES pad. L2 and R2 are real keys, the d-pad is four
 * keys rather than a hat, and the key the device calls FUNCTION
 * (BTN_TRIGGER_HAPPY1) is MENU. */
static const struct { int code; int btn; } keymap[] = {
	{ BTN_DPAD_UP,    DIATOM_BTN_UP },    { BTN_DPAD_DOWN,  DIATOM_BTN_DOWN },
	{ BTN_DPAD_LEFT,  DIATOM_BTN_LEFT },  { BTN_DPAD_RIGHT, DIATOM_BTN_RIGHT },
	{ BTN_SOUTH,      DIATOM_BTN_A },     { BTN_EAST,       DIATOM_BTN_B },
	{ BTN_NORTH,      DIATOM_BTN_X },     { BTN_WEST,       DIATOM_BTN_Y },
	{ BTN_TL,         DIATOM_BTN_L1 },    { BTN_TR,         DIATOM_BTN_R1 },
	{ BTN_TL2,        DIATOM_BTN_L2 },    { BTN_TR2,        DIATOM_BTN_R2 },
	{ BTN_SELECT,     DIATOM_BTN_SELECT },{ BTN_START,      DIATOM_BTN_START },
	{ BTN_TRIGGER_HAPPY1, DIATOM_BTN_MENU },
};

/* ---------- audio ----------------------------------------------------------- */

static SDL_AudioDeviceID g_audio;
static char              g_audio_dev[128];

static void port_logf(diatom_log_level lvl, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void port_logf(diatom_log_level lvl, const char *fmt, ...)
{
	char msg[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	diatom_port_log(lvl, msg);
}

/* ==================== levels ================================================ */

/* Is there a plug in the headphone jack? SW_HEADPHONE_INSERT on the codec's
 * input device ("rk817_int Headphones"), found by capability as on the Brick. */
static void jack_open(void)
{
	unsigned long bits[NLONGS(SW_MAX)];
	char path[32];
	int i, fd;

	for (i = 0; i < 32; i++) {
		snprintf(path, sizeof path, "/dev/input/event%d", i);
		if ((fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) < 0) continue;
		memset(bits, 0, sizeof bits);
		if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof bits), bits) >= 0 &&
		    BIT_IS_SET(bits, SW_HEADPHONE_INSERT)) {
			g_jack_fd = fd;
			return;
		}
		close(fd);
	}
	diatom_port_log(DIATOM_LOG_WARN, "pixel2: no headphone jack input; "
	                "volume uses the speaker window");
}

static int jack_present(void)
{
	unsigned long bits[NLONGS(SW_MAX)];

	if (g_jack_fd < 0) return 0;
	memset(bits, 0, sizeof bits);
	if (ioctl(g_jack_fd, EVIOCGSW(sizeof bits), bits) < 0) return 0;
	return BIT_IS_SET(bits, SW_HEADPHONE_INSERT) ? 1 : 0;
}

static int att_top(void) { return jack_present() ? HP_ATT_TOP    : SPK_ATT_TOP; }
static int att_bot(void) { return jack_present() ? HP_ATT_BOTTOM : SPK_ATT_BOTTOM; }

/* brick.c's arithmetic on attenuation, which is also TortOS's
 * aout_level_to_raw: level 20 at the top of the window, level 0 at its
 * bottom, rounded both ways so a read-back lands on the level it came from.
 * Level 0 is then replaced by the cut. */
static int level_to_raw(int lv)
{
	int top = att_top(), span = att_bot() - top, att;

	if (lv <= 0) return 0;
	if (lv > GAIN_LEVELS) lv = GAIN_LEVELS;
	att = top + ((GAIN_LEVELS - lv) * span + GAIN_LEVELS / 2) / GAIN_LEVELS;
	return GAIN_RAW_MAX - att;
}

static int raw_to_level(int raw)
{
	int top = att_top(), bot = att_bot(), span = bot - top;
	int att = GAIN_RAW_MAX - raw;

	/* Below the window reads as silence, above it as the top: a register
	 * left outside it was set by something that was not us. */
	if (att >= bot) return 0;
	if (att <= top) return GAIN_LEVELS;
	return ((bot - att) * GAIN_LEVELS + span / 2) / span;
}

static int gain_io(long *val, int write)
{
	struct dm_ctl_elem_value v;

	if (g_mixer_fd < 0) return -1;
	memset(&v, 0, sizeof v);
	v.id.iface = 2;                                 /* SNDRV_CTL_ELEM_IFACE_MIXER */
	snprintf((char *)v.id.name, sizeof v.id.name, "%s", GAIN_CTL);
	if (write) {
		/* Both channels: a stereo control written in one leaves the other
		 * where it was, which reads as a balance problem. */
		v.value.integer.value[0] = *val;
		v.value.integer.value[1] = *val;
		if (ioctl(g_mixer_fd, DM_CTL_ELEM_WRITE, &v) < 0) {
			port_logf(DIATOM_LOG_WARN, "pixel2: mixer rejected '%s' = %ld",
			          GAIN_CTL, *val);
			return -1;
		}
		return 0;
	}
	if (ioctl(g_mixer_fd, DM_CTL_ELEM_READ, &v) < 0) return -1;
	*val = v.value.integer.value[0];
	return 0;
}

static void gain_ensure(void)
{
	long v;

	if (g_level >= 0) return;
	if (gain_io(&v, 0) < 0) return;
	g_level = raw_to_level((int)v);
}

static int g_jack_was = -1;         /* jack state at the last write; -1 = never */

static void gain_apply(void)
{
	long v = (g_level <= 0 || g_muted) ? 0 : level_to_raw(g_level);

	g_jack_was = jack_present();
	gain_io(&v, 1);
}

/* Re-apply when the plug goes in or out, as on the Brick: whoever pumps input
 * owns the level, and the windows differ by jack (or will, once measured). */
static void gain_jack_poll(void)
{
	if (jack_present() == g_jack_was || g_level < 0) return;
	gain_apply();
}

static void osd_show(int kind, int level, int max)
{
	pthread_mutex_lock(&g_mx);
	g_osd_kind  = kind;
	g_osd_level = level;
	g_osd_max   = max;
	g_osd_until = diatom_port_now_us() + 1500000ull;
	pthread_mutex_unlock(&g_mx);
}

static void gain_nudge(int dir)
{
	gain_ensure();
	if (g_level < 0) return;
	g_level += dir;
	if (g_level < 0)           g_level = 0;
	if (g_level > GAIN_LEVELS) g_level = GAIN_LEVELS;
	gain_apply();
	osd_show(0, g_level, GAIN_LEVELS);
}

static int backlight_read(void)
{
	char buf[16];
	ssize_t n;
	int fd = open(BACKLIGHT, O_RDONLY | O_CLOEXEC);

	if (fd < 0) return -1;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n <= 0) return -1;
	buf[n] = '\0';
	return atoi(buf);
}

static void backlight_write(int raw)
{
	char buf[16];
	int fd = open(BACKLIGHT, O_WRONLY | O_CLOEXEC);
	int n = snprintf(buf, sizeof buf, "%d\n", raw);

	if (fd < 0) return;
	if (write(fd, buf, (size_t)n) != n)
		diatom_port_log(DIATOM_LOG_WARN, "pixel2: backlight write failed");
	close(fd);
}

static void bright_ensure(void)
{
	int raw, i;

	if (g_bright >= 0) return;
	if ((raw = backlight_read()) < 0) return;
	/* Nearest rung, so the first press moves one step from where the launcher
	 * left the panel rather than jumping. */
	g_bright = 0;
	for (i = 1; i <= BRIGHT_LEVELS; i++)
		if (abs(bright_ladder[i] - raw) < abs(bright_ladder[g_bright] - raw))
			g_bright = i;
}

static void bright_nudge(int dir)
{
	bright_ensure();
	if (g_bright < 0) return;
	g_bright += dir;
	if (g_bright < 0)             g_bright = 0;
	if (g_bright > BRIGHT_LEVELS) g_bright = BRIGHT_LEVELS;
	backlight_write(bright_ladder[g_bright]);
	osd_show(1, g_bright, BRIGHT_LEVELS);
}

/* ==================== GL ==================================================== */

static const char *VERTEX_SHADER =
	"#version 300 es\n"
	"in vec2 a_pos;\n"
	"in vec2 a_src;\n"
	"out vec2 v_src;\n"
	"void main() { v_src = a_src; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

/* Nearest: the sampler's own, which picks the source pixel under the
 * destination pixel's centre - floor((i + 0.5) / scale), the same pixel
 * brick.c's map picks. */
static const char *PLAIN_SHADER =
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec2 u_size;\n"
	"in vec2 v_src;\n"
	"out vec4 o_color;\n"
	"void main() { o_color = texture(u_tex, v_src / u_size); }\n";

/* Sharp-bilinear, brick.c's build_map in a shader: the bilinear weight,
 * steepened by the scale so the blend spans one destination pixel. Interior
 * pixels sample a texel centre exactly; only the pixel straddling a source
 * boundary mixes. At an integer factor every weight is 0 or 1, so it equals
 * nearest. The edges need no clamp of their own: CLAMP_TO_EDGE blends the
 * border texel with itself, which is brick.c's clamp. UNVERIFIED against
 * brick.c's output until a capture comparison (ADR-0035). */
static const char *SHARP_SHADER =
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec2 u_size;\n"
	"uniform vec2 u_scale;\n"
	"in vec2 v_src;\n"
	"out vec4 o_color;\n"
	"void main() {\n"
	"	vec2 c = v_src - 0.5;\n"
	"	vec2 p = floor(c);\n"
	"	vec2 f = clamp((c - p - 0.5) * u_scale + 0.5, 0.0, 1.0);\n"
	"	o_color = texture(u_tex, (p + 0.5 + f) / u_size);\n"
	"}\n";

static const char *SOLID_SHADER =
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform vec4 u_color;\n"
	"out vec4 o_color;\n"
	"void main() { o_color = u_color; }\n";

static GLuint compile(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(s, sizeof log, NULL, log);
		port_logf(DIATOM_LOG_ERROR, "pixel2: shader: %s", log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static GLuint link_program(const char *fragment)
{
	GLuint vs = compile(GL_VERTEX_SHADER, VERTEX_SHADER);
	GLuint fs = compile(GL_FRAGMENT_SHADER, fragment);
	GLuint p;
	GLint ok = 0;

	if (!vs || !fs) return 0;
	p = glCreateProgram();
	glAttachShader(p, vs);
	glAttachShader(p, fs);
	glBindAttribLocation(p, 0, "a_pos");
	glBindAttribLocation(p, 1, "a_src");
	glLinkProgram(p);
	glDeleteShader(vs);
	glDeleteShader(fs);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		diatom_port_log(DIATOM_LOG_ERROR, "pixel2: program did not link");
		glDeleteProgram(p);
		return 0;
	}
	return p;
}

/* A rect in SURFACE coordinates (landscape, y down) to clip space on the
 * target being drawn. On glass the panel is portrait and the picture turns
 * 90 degrees counter-clockwise, which on this panel ("Left Side Up") is the
 * right way up - checked by eye 2026-10-01 with a marker drawn top-left.
 * Landscape (x, y) lands on portrait (y, W - x). For a capture the target is
 * the surface itself and nothing turns. */
static void to_clip(float x, float y, bool turned, float *out)
{
	if (turned) {
		float px = y, py = (float)g_surface_w - x;
		out[0] = px / (float)g_panel_w * 2.0f - 1.0f;
		out[1] = 1.0f - py / (float)g_panel_h * 2.0f;
	} else {
		out[0] = x / (float)g_surface_w * 2.0f - 1.0f;
		out[1] = 1.0f - y / (float)g_surface_h * 2.0f;
	}
}

/* One textured or solid quad. `src` is in texels, matching the rect's corners:
 * top-left, top-right, bottom-left, bottom-right. */
static void draw_quad(float x, float y, float w, float h,
                      float sw, float sh, bool turned)
{
	float pos[8], src[8] = { 0, 0, sw, 0, 0, sh, sw, sh };

	to_clip(x,     y,     turned, &pos[0]);
	to_clip(x + w, y,     turned, &pos[2]);
	to_clip(x,     y + h, turned, &pos[4]);
	to_clip(x + w, y + h, turned, &pos[6]);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, src);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void solid(float x, float y, float w, float h,
                  float r, float g, float b, float a, bool turned)
{
	glUseProgram(g_prog_solid);
	glUniform4f(glGetUniformLocation(g_prog_solid, "u_color"), r, g, b, a);
	draw_quad(x, y, w, h, 0, 0, turned);
}

/* Upload a mailbox frame into the frame texture. RGB565 goes up as is;
 * XRGB8888 goes up as bytes and the texture's swizzle puts B and R back and
 * pins alpha, so no shader needs to know which format it is drawing. */
static void upload_frame(const mailbox_frame *f)
{
	size_t bpp = (f->fmt == DIATOM_PIX_RGB565) ? 2 : 4;
	bool realloc_tex = f->w != g_tex_w || f->h != g_tex_h || f->fmt != g_tex_fmt;

	glBindTexture(GL_TEXTURE_2D, g_frame_tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(f->pitch / bpp));
	if (f->fmt == DIATOM_PIX_RGB565) {
		if (realloc_tex)
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB565, f->w, f->h, 0,
			             GL_RGB, GL_UNSIGNED_SHORT_5_6_5, f->pixels);
		else
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f->w, f->h,
			                GL_RGB, GL_UNSIGNED_SHORT_5_6_5, f->pixels);
	} else {
		if (realloc_tex) {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, f->w, f->h, 0,
			             GL_RGBA, GL_UNSIGNED_BYTE, f->pixels);
		} else {
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f->w, f->h,
			                GL_RGBA, GL_UNSIGNED_BYTE, f->pixels);
		}
	}
	if (realloc_tex) {
		bool xrgb = f->fmt == DIATOM_PIX_XRGB8888;
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, xrgb ? GL_BLUE : GL_RED);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, xrgb ? GL_RED : GL_BLUE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
		g_tex_w = f->w;
		g_tex_h = f->h;
		g_tex_fmt = f->fmt;
	}
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

/* The level bar, the Brick's design so it reads the same on both devices and
 * on the shelf: a scrim across the top of the surface with a 6 px bar in it,
 * in the launcher's two colors. Blended, so "halve what is there" is black at
 * half alpha. */
static void draw_level_bar(int kind, int level, int max, bool turned)
{
	const float pad = 3, bar = 6, W = (float)g_surface_w;
	float fill = (level < 0 || max <= 0) ? 0 : W * (float)level / (float)max;

	solid(0, 0, W, pad, 0, 0, 0, 0.5f, turned);
	solid(0, pad + bar, W, pad, 0, 0, 0, 0.5f, turned);
	if (fill > 0) {
		if (kind) solid(0, pad, fill, bar, 255 / 255.f, 206 / 255.f, 128 / 255.f, 1, turned);
		else      solid(0, pad, fill, bar,  61 / 255.f, 214 / 255.f, 255 / 255.f, 1, turned);
	}
	if (fill < W)
		solid(fill, pad, W - fill, bar, 60 / 255.f, 62 / 255.f, 72 / 255.f, 1, turned);
}

/* Draw one frame and whatever is over it, onto the current target. */
static void draw_scene(const mailbox_frame *f, bool overlay_live, bool osd_live,
                       int osd_kind, int osd_level, int osd_max, bool turned)
{
	GLuint prog = f->filter == DIATOM_FILTER_SHARP ? g_prog_sharp : g_prog_plain;
	GLint  filter = f->filter == DIATOM_FILTER_SHARP ? GL_LINEAR : GL_NEAREST;

	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);

	glBindTexture(GL_TEXTURE_2D, g_frame_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glUseProgram(prog);
	glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
	glUniform2f(glGetUniformLocation(prog, "u_size"), (float)f->w, (float)f->h);
	if (prog == g_prog_sharp)
		glUniform2f(glGetUniformLocation(prog, "u_scale"),
		            (float)f->dst.w / (float)f->w, (float)f->dst.h / (float)f->h);
	/* dst may reach past the surface (fill, overscale): GL clips it. */
	draw_quad((float)f->dst.x, (float)f->dst.y, (float)f->dst.w, (float)f->dst.h,
	          (float)f->w, (float)f->h, turned);

	/* Alpha in the target stays 1 whatever is blended over it: the scanout
	 * format carries alpha, and this display controller is not trusted to
	 * ignore it (the Brick's composited per pixel - see brick.c, g_opaque). */
	glEnable(GL_BLEND);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
	if (overlay_live) {
		float x0 = (float)((g_surface_w - g_ov_w) / 2);
		float y0 = (float)(g_surface_h - g_ov_h - g_surface_h / 24);

		glBindTexture(GL_TEXTURE_2D, g_overlay_tex);
		glUseProgram(g_prog_plain);
		glUniform1i(glGetUniformLocation(g_prog_plain, "u_tex"), 0);
		glUniform2f(glGetUniformLocation(g_prog_plain, "u_size"),
		            (float)g_ov_w, (float)g_ov_h);
		draw_quad(x0, y0, (float)g_ov_w, (float)g_ov_h,
		          (float)g_ov_w, (float)g_ov_h, turned);
	}
	if (osd_live)
		draw_level_bar(osd_kind, osd_level, osd_max, turned);
	glDisable(GL_BLEND);
}

/* ==================== KMS =================================================== */

static void bo_destroyed(struct gbm_bo *bo, void *data)
{
	uint32_t fb = (uint32_t)(uintptr_t)data;

	if (fb) drmModeRmFB(gbm_device_get_fd(gbm_bo_get_device(bo)), fb);
}

/* The framebuffer for a GBM buffer, made once and kept with it. ARGB8888,
 * because a page flip cannot change format and the launcher's SDL scans out
 * ARGB8888 (ADR-0036, measured as EINVAL otherwise). */
static uint32_t bo_fb(struct gbm_bo *bo)
{
	uint32_t fb = (uint32_t)(uintptr_t)gbm_bo_get_user_data(bo);
	uint32_t handles[4] = { gbm_bo_get_handle(bo).u32 };
	uint32_t pitches[4] = { gbm_bo_get_stride(bo) };
	uint32_t offsets[4] = { 0 };

	if (fb) return fb;
	if (drmModeAddFB2(g_drm_fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo),
	                  DRM_FORMAT_ARGB8888, handles, pitches, offsets, &fb, 0)) {
		port_logf(DIATOM_LOG_ERROR, "pixel2: AddFB2: %s", strerror(errno));
		return 0;
	}
	gbm_bo_set_user_data(bo, (void *)(uintptr_t)fb, bo_destroyed);
	return fb;
}

/* Take the display, if it is not ours already, and find out whether anything
 * is scanning out (ADR-0036). With the launcher or the boot splash in front,
 * the CRTC is lit and a flip is all it takes; standalone, nothing is, and the
 * first frame sets the mode. */
static bool take_display(void)
{
	drmModeCrtc *crtc;

	if (g_have_master) return true;
	if (drmSetMaster(g_drm_fd) != 0) {
		port_logf(DIATOM_LOG_ERROR, "pixel2: cannot take the display: %s",
		          strerror(errno));
		return false;
	}
	g_have_master = true;
	crtc = drmModeGetCrtc(g_drm_fd, g_crtc_id);
	g_crtc_lit = crtc && crtc->mode_valid && crtc->buffer_id;
	drmModeFreeCrtc(crtc);
	return true;
}

static void give_display(void)
{
	if (!g_have_master) return;
	if (drmDropMaster(g_drm_fd) != 0)
		port_logf(DIATOM_LOG_WARN, "pixel2: drop master: %s", strerror(errno));
	g_have_master = false;
}

static void flip_done(int fd, unsigned seq, unsigned sec, unsigned usec, void *data)
{
	*(bool *)data = true;
}

static void wait_vblank(void)
{
	drmVBlank vbl;

	memset(&vbl, 0, sizeof vbl);
	vbl.request.type = DRM_VBLANK_RELATIVE;
	if (g_crtc_pipe == 1)
		vbl.request.type |= DRM_VBLANK_SECONDARY;
	else if (g_crtc_pipe > 1)
		vbl.request.type |= (g_crtc_pipe << DRM_VBLANK_HIGH_CRTC_SHIFT) &
		                    DRM_VBLANK_HIGH_CRTC_MASK;
	vbl.request.sequence = 1;
	drmWaitVBlank(g_drm_fd, &vbl);
}

/* Put what was just swapped on glass, and wait until it is. */
static void show_frame(void)
{
	struct gbm_bo *bo = gbm_surface_lock_front_buffer(g_gbm_surface);
	uint32_t fb;
	bool done = false;
	int tries = 0, ret;

	if (!bo) {
		diatom_port_log(DIATOM_LOG_ERROR, "pixel2: no front buffer after swap");
		return;
	}
	if (!(fb = bo_fb(bo)) || !take_display()) {
		gbm_surface_release_buffer(g_gbm_surface, bo);
		return;
	}

	if (!g_crtc_lit) {
		/* Standalone: nothing was lit, so this frame sets the mode. */
		if (drmModeSetCrtc(g_drm_fd, g_crtc_id, fb, 0, 0, &g_connector_id, 1,
		                   &g_mode) != 0) {
			port_logf(DIATOM_LOG_ERROR, "pixel2: set mode: %s", strerror(errno));
			gbm_surface_release_buffer(g_gbm_surface, bo);
			return;
		}
		g_crtc_lit = true;
		done = true;
	} else {
		/* The previous owner's last frame may still be queued, and the flip
		 * comes back busy until it lands: measured every time, one refresh
		 * (ADR-0036). Wait a refresh and retry, a few times at most. */
		while ((ret = drmModePageFlip(g_drm_fd, g_crtc_id, fb,
		                              DRM_MODE_PAGE_FLIP_EVENT, &done)) != 0 &&
		       errno == EBUSY && tries++ < 5)
			wait_vblank();
		if (ret != 0) {
			port_logf(DIATOM_LOG_WARN, "pixel2: page flip: %s", strerror(errno));
			gbm_surface_release_buffer(g_gbm_surface, bo);
			return;
		}
	}

	while (!done) {
		struct pollfd p = { .fd = g_drm_fd, .events = POLLIN };
		drmEventContext ev = { .version = 2, .page_flip_handler = flip_done };

		/* Bounded, so a flip that never completes costs a frame and a log
		 * line rather than the thread. */
		if (poll(&p, 1, 100) <= 0) {
			diatom_port_log(DIATOM_LOG_WARN, "pixel2: flip did not complete");
			break;
		}
		drmHandleEvent(g_drm_fd, &ev);
	}

	if (g_shown_bo) gbm_surface_release_buffer(g_gbm_surface, g_shown_bo);
	g_shown_bo = bo;
}

/* ==================== the presenting thread ================================= */

static bool gl_start(void)
{
	static const EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_NONE
	};
	static const EGLint context_attribs[] = {
		EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE
	};
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLConfig configs[64], config = NULL;
	EGLint n = 0, i;

	g_egl_display = get_platform_display
		? get_platform_display(EGL_PLATFORM_GBM_KHR, g_gbm, NULL)
		: eglGetDisplay((EGLNativeDisplayType)g_gbm);
	if (g_egl_display == EGL_NO_DISPLAY || !eglInitialize(g_egl_display, NULL, NULL)) {
		diatom_port_log(DIATOM_LOG_ERROR, "pixel2: no EGL display");
		return false;
	}
	eglBindAPI(EGL_OPENGL_ES_API);

	/* The config whose native format is the scanout format exactly. */
	eglChooseConfig(g_egl_display, config_attribs, configs, 64, &n);
	for (i = 0; i < n; i++) {
		EGLint visual = 0;
		eglGetConfigAttrib(g_egl_display, configs[i], EGL_NATIVE_VISUAL_ID, &visual);
		if (visual == GBM_FORMAT_ARGB8888) { config = configs[i]; break; }
	}
	if (!config) {
		diatom_port_log(DIATOM_LOG_ERROR, "pixel2: no ARGB8888 EGL config");
		return false;
	}

	g_gbm_surface = gbm_surface_create(g_gbm, (uint32_t)g_panel_w, (uint32_t)g_panel_h,
	                                   GBM_FORMAT_ARGB8888,
	                                   GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (!g_gbm_surface) {
		diatom_port_log(DIATOM_LOG_ERROR, "pixel2: no GBM surface");
		return false;
	}
	g_egl_context = eglCreateContext(g_egl_display, config, EGL_NO_CONTEXT, context_attribs);
	g_egl_surface = eglCreateWindowSurface(g_egl_display, config,
	                                       (EGLNativeWindowType)g_gbm_surface, NULL);
	if (g_egl_context == EGL_NO_CONTEXT || g_egl_surface == EGL_NO_SURFACE ||
	    !eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface, g_egl_context)) {
		diatom_port_log(DIATOM_LOG_ERROR, "pixel2: no GLES 3 context");
		return false;
	}

	g_prog_plain = link_program(PLAIN_SHADER);
	g_prog_sharp = link_program(SHARP_SHADER);
	g_prog_solid = link_program(SOLID_SHADER);
	if (!g_prog_plain || !g_prog_sharp || !g_prog_solid) return false;

	glGenTextures(1, &g_frame_tex);
	glGenTextures(1, &g_overlay_tex);
	glBindTexture(GL_TEXTURE_2D, g_frame_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	/* The overlay is BGRA straight alpha, uploaded as bytes: swizzled back. */
	glBindTexture(GL_TEXTURE_2D, g_overlay_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
	glViewport(0, 0, g_panel_w, g_panel_h);
	return true;
}

static void gl_stop(void)
{
	if (g_egl_display == EGL_NO_DISPLAY) return;
	eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (g_egl_surface != EGL_NO_SURFACE) eglDestroySurface(g_egl_display, g_egl_surface);
	if (g_egl_context != EGL_NO_CONTEXT) eglDestroyContext(g_egl_display, g_egl_context);
	eglTerminate(g_egl_display);
	g_egl_display = EGL_NO_DISPLAY;
}

/* A capture: the front frame drawn again, unturned, into an offscreen target
 * the size of the surface, and read back. What the host asked for is the
 * picture as the player sees it, which is landscape. */
static bool capture_front(int front, const char *path)
{
	GLuint fbo, rb;
	uint8_t *rgba, *flipped;
	SDL_Surface *s, *rgb;
	size_t row = (size_t)g_surface_w * 4;
	bool ok = false;
	int y;

	if (front < 0) return false;
	rgba = malloc(row * (size_t)g_surface_h);
	flipped = malloc(row * (size_t)g_surface_h);
	if (!rgba || !flipped) { free(rgba); free(flipped); return false; }

	glGenFramebuffers(1, &fbo);
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, g_surface_w, g_surface_h);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
	glViewport(0, 0, g_surface_w, g_surface_h);

	upload_frame(&g_frames[front]);
	draw_scene(&g_frames[front], false, false, 0, 0, 0, false);
	glReadPixels(0, 0, g_surface_w, g_surface_h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, g_panel_w, g_panel_h);
	glDeleteRenderbuffers(1, &rb);
	glDeleteFramebuffers(1, &fbo);

	/* GL reads bottom row first. */
	for (y = 0; y < g_surface_h; y++)
		memcpy(flipped + (size_t)y * row, rgba + (size_t)(g_surface_h - 1 - y) * row, row);

	s = SDL_CreateRGBSurfaceWithFormatFrom(flipped, g_surface_w, g_surface_h, 32,
	                                       (int)row, SDL_PIXELFORMAT_ABGR8888);
	if (s) {
		/* Plain 24-bit, as on the Brick: readers refuse 32-bit BMP headers. */
		rgb = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_RGB24, 0);
		if (rgb) { ok = SDL_SaveBMP(rgb, path) == 0; SDL_FreeSurface(rgb); }
		SDL_FreeSurface(s);
	}
	free(rgba);
	free(flipped);
	return ok;
}

static void *present_thread(void *arg)
{
	bool ok = gl_start();

	pthread_mutex_lock(&g_mx);
	g_thread_ready = ok ? 1 : -1;
	pthread_cond_broadcast(&g_idle);
	if (!ok) { pthread_mutex_unlock(&g_mx); gl_stop(); return arg; }

	while (!g_thread_stop) {
		int frame;
		bool ov_live, osd_live;
		int osd_kind, osd_level, osd_max;
		uint64_t now, t0 = 0;

		if (g_pending < 0 && !g_want_blank && !g_capture_path) {
			pthread_cond_wait(&g_cv, &g_mx);
			continue;
		}

		if (g_capture_path) {
			const char *path = g_capture_path;
			int front = g_front;

			pthread_mutex_unlock(&g_mx);
			ok = capture_front(front, path);
			pthread_mutex_lock(&g_mx);
			g_capture_ok = ok;
			g_capture_path = NULL;
			pthread_cond_broadcast(&g_idle);
			continue;
		}

		if (g_want_blank) {
			g_want_blank = false;
			g_blanking = true;
			pthread_mutex_unlock(&g_mx);
			glClearColor(0, 0, 0, 1);
			glClear(GL_COLOR_BUFFER_BIT);
			eglSwapBuffers(g_egl_display, g_egl_surface);
			show_frame();
			pthread_mutex_lock(&g_mx);
			g_blanking = false;
			pthread_cond_broadcast(&g_idle);
			continue;
		}

		frame = g_pending;
		g_pending  = -1;
		g_inflight = frame;

		/* The overlay is read here, under the lock the host's next
		 * diatom_port_overlay takes, so the borrowed pointer is never read
		 * after the host has moved on. */
		now = diatom_port_now_us();
		ov_live = g_ov && now < g_ov_until;
		if (ov_live && g_ov_dirty) {
			glBindTexture(GL_TEXTURE_2D, g_overlay_tex);
			glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_ov_w, g_ov_h, 0,
			             GL_RGBA, GL_UNSIGNED_BYTE, g_ov);
			g_ov_dirty = false;
		}
		osd_live  = now < g_osd_until;
		osd_kind  = g_osd_kind;
		osd_level = g_osd_level;
		osd_max   = g_osd_max;
		pthread_mutex_unlock(&g_mx);

		if (g_present_debug) t0 = diatom_port_now_us();
		upload_frame(&g_frames[frame]);
		draw_scene(&g_frames[frame], ov_live, osd_live, osd_kind, osd_level,
		           osd_max, g_turned);
		eglSwapBuffers(g_egl_display, g_egl_surface);
		if (g_present_debug) {
			static int n;
			uint64_t t1 = diatom_port_now_us();
			if (++n >= 60) {
				n = 0;
				port_logf(DIATOM_LOG_DEBUG, "present: upload+draw+swap %llu us",
				          (unsigned long long)(t1 - t0));
			}
		}
		show_frame();

		pthread_mutex_lock(&g_mx);
		g_front    = frame;
		g_inflight = -1;
		pthread_cond_broadcast(&g_idle);
	}
	pthread_mutex_unlock(&g_mx);
	gl_stop();
	return arg;
}

/* Until nothing is pending, nothing is being drawn and no request is open.
 * The thread keeps running: callers want quiescence, not teardown. */
static void drain(void)
{
	pthread_mutex_lock(&g_mx);
	while (g_thread_running &&
	       (g_pending >= 0 || g_inflight >= 0 || g_want_blank || g_blanking ||
	        g_capture_path))
		pthread_cond_wait(&g_idle, &g_mx);
	pthread_mutex_unlock(&g_mx);
}

/* ==================== the port interface ==================================== */

void diatom_port_present(const void *src, int w, int h, size_t pitch,
                         diatom_pixfmt fmt, diatom_rect dst, diatom_filter filter)
{
	mailbox_frame *f;
	size_t need;
	int i;

	/* Dupe frame: the panel already shows it. */
	if (!src || w <= 0 || h <= 0 || !g_thread_running) return;

	pthread_mutex_lock(&g_mx);
	for (i = 0; i < MAILBOX_FRAMES; i++)
		if (i != g_front && i != g_inflight && i != g_pending) break;
	if (i == MAILBOX_FRAMES) {
		i = g_pending;
		g_pending = -1;
	}
	pthread_mutex_unlock(&g_mx);

	f = &g_frames[i];
	need = pitch * (size_t)h;
	if (need > f->cap) {
		/* Grows only, and only when a core's geometry outgrows every frame so
		 * far: a few times per game at most, never per frame. */
		uint8_t *p = realloc(f->pixels, need);
		if (!p) {
			diatom_port_log(DIATOM_LOG_WARN, "present: no memory for the frame; dropped");
			return;
		}
		f->pixels = p;
		f->cap = need;
	}
	/* What the port was handed, once per change, as brick.c logs it. Against
	 * the last frame logged, not the slot's old contents: the slots rotate. */
	{
		static int lw = -1, lh = -1;
		static size_t lpitch;
		static diatom_rect ldst;
		static diatom_filter lfilter;

		if (w != lw || h != lh || pitch != lpitch || filter != lfilter ||
		    memcmp(&dst, &ldst, sizeof dst) != 0) {
			lw = w; lh = h; lpitch = pitch; ldst = dst; lfilter = filter;
			port_logf(DIATOM_LOG_INFO,
			          "present: src %dx%d pitch %zu fmt %d -> dst %dx%d at %d,%d "
			          "(surface %dx%d) %s",
			          w, h, pitch, (int)fmt, dst.w, dst.h, dst.x, dst.y,
			          g_surface_w, g_surface_h,
			          filter == DIATOM_FILTER_SHARP ? "sharp" : "nearest");
		}
	}
	memcpy(f->pixels, src, need);
	f->w = w;
	f->h = h;
	f->pitch = pitch;
	f->fmt = fmt;
	f->dst = dst;
	f->filter = filter;

	pthread_mutex_lock(&g_mx);
	g_presented = true;
	g_pending = i;
	pthread_cond_signal(&g_cv);
	pthread_mutex_unlock(&g_mx);
}

void diatom_port_overlay(const uint8_t *bgra, int w, int h, unsigned ms)
{
	pthread_mutex_lock(&g_mx);
	if (!bgra || ms == 0 || w <= 0 || h <= 0) {
		g_ov = NULL;
		g_ov_until = 0;
	} else if (w > g_surface_w || h > g_surface_h) {
		/* Refused, not clipped: see diatom_port.h. */
		diatom_port_log(DIATOM_LOG_WARN, "overlay larger than the surface; ignored");
	} else {
		g_ov = bgra;
		g_ov_w = w;
		g_ov_h = h;
		g_ov_until = diatom_port_now_us() + (uint64_t)ms * 1000ull;
		g_ov_dirty = true;
	}
	pthread_mutex_unlock(&g_mx);
}

void diatom_port_present_stop(diatom_park park)
{
	bool was_presenting;

	if (!g_thread_running) return;
	drain();

	pthread_mutex_lock(&g_mx);
	was_presenting = g_presented;
	g_presented = false;
	pthread_mutex_unlock(&g_mx);

	/* Nothing presented since the last stop: the launcher has the display,
	 * and parking would take it (the Brick learned this the hard way, see
	 * brick.c). */
	if (!was_presenting) return;

	if (park == DIATOM_PARK_BLANK) {
		pthread_mutex_lock(&g_mx);
		g_want_blank = true;
		pthread_cond_signal(&g_cv);
		pthread_mutex_unlock(&g_mx);
		drain();
	}
	/* KEEP leaves the last frame on glass, and the buffer behind it stays
	 * allocated: removing a framebuffer that is on glass blanks the panel
	 * (ADR-0036). The launcher's first flip replaces it. */
	give_display();
}

/* ---------- audio, as on the Brick (ADR-0029, ADR-0030) --------------------- */

static bool audio_open(const char *name)
{
	SDL_AudioSpec want, have;

	if (g_audio) { SDL_CloseAudioDevice(g_audio); g_audio = 0; }
	if (name && *name) setenv("AUDIODEV", name, 1);
	else               unsetenv("AUDIODEV");

	SDL_memset(&want, 0, sizeof want);
	want.freq     = AUDIO_RATE;
	want.format   = AUDIO_S16SYS;
	want.channels = 2;
	want.samples  = 1024;

	if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
		return false;
	g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (!g_audio) return false;
	SDL_PauseAudioDevice(g_audio, 0);
	port_logf(DIATOM_LOG_INFO,
	          "audio: opened %s at %d Hz, %d ch, %d frames/period, %u byte buffer",
	          name && *name ? name : "the default device",
	          have.freq, have.channels, have.samples, have.size);
	return true;
}

bool diatom_port_audio_set(const char *name, char *actual, size_t cap)
{
	const char *want = name ? name : "";
	bool ok;

	if (g_audio && !strcmp(want, g_audio_dev)) {
		diatom_port_audio_get(actual, cap);
		return true;
	}
	ok = audio_open(name);
	if (!ok) {
		port_logf(DIATOM_LOG_WARN, "audio: %s unavailable (%s); using the default",
		          name && *name ? name : "the default device", SDL_GetError());
		if (audio_open(NULL)) g_audio_dev[0] = '\0';
	} else {
		snprintf(g_audio_dev, sizeof g_audio_dev, "%s", name ? name : "");
	}
	diatom_port_audio_get(actual, cap);
	return ok;
}

void diatom_port_audio_get(char *out, size_t cap)
{
	if (out && cap) snprintf(out, cap, "%s", g_audio_dev);
}

size_t diatom_port_audio_write(const int16_t *frames, size_t n)
{
	size_t queued, room;

	if (g_audio && g_audio_dev[0] &&
	    SDL_GetAudioDeviceStatus(g_audio) == SDL_AUDIO_STOPPED) {
		diatom_port_log(DIATOM_LOG_WARN,
		                "audio: the sink went away; falling back to the default");
		g_audio_dev[0] = '\0';
		audio_open(NULL);
	}
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

/* ---------- input ----------------------------------------------------------- */

static int open_by_key(int code, int skip_fd)
{
	unsigned long bits[NLONGS(KEY_MAX)];
	char path[32];
	int i, fd;

	for (i = 0; i < 32; i++) {
		snprintf(path, sizeof path, "/dev/input/event%d", i);
		if ((fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) < 0) continue;
		memset(bits, 0, sizeof bits);
		if (fd != skip_fd && ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) >= 0 &&
		    BIT_IS_SET(bits, code))
			return fd;
		close(fd);
	}
	return -1;
}

/* The pad's keys as they are right now, for a start or after the kernel
 * dropped events (SYN_DROPPED): the held-at-entry rule needs to know about a
 * key that was already down. */
static void pad_resync(void)
{
	unsigned long keys[NLONGS(KEY_MAX)];
	size_t i;

	if (g_pad_fd < 0) return;
	memset(keys, 0, sizeof keys);
	if (ioctl(g_pad_fd, EVIOCGKEY(sizeof keys), keys) < 0) return;
	g_buttons = 0;
	for (i = 0; i < sizeof keymap / sizeof keymap[0]; i++)
		if (BIT_IS_SET(keys, keymap[i].code))
			g_buttons |= DIATOM_BIT(keymap[i].btn);
}

static void input_open(void)
{
	g_pad_fd  = open_by_key(BTN_SOUTH, -1);
	g_keys_fd = open_by_key(KEY_VOLUMEUP, g_pad_fd);
	if (g_pad_fd < 0)
		diatom_port_log(DIATOM_LOG_WARN, "pixel2: no gamepad found; no game input");
	if (g_keys_fd < 0)
		diatom_port_log(DIATOM_LOG_WARN, "pixel2: no volume keys found");
	pad_resync();
}

void diatom_port_input_poll(void)
{
	struct input_event ev[32];
	ssize_t n;
	size_t i, k;

	gain_jack_poll();

	while (g_pad_fd >= 0 && (n = read(g_pad_fd, ev, sizeof ev)) > 0) {
		for (i = 0; i < (size_t)n / sizeof ev[0]; i++) {
			if (ev[i].type == EV_SYN && ev[i].code == SYN_DROPPED) {
				pad_resync();
				continue;
			}
			if (ev[i].type != EV_KEY || ev[i].value == 2) continue;
			if (g_input_debug)
				port_logf(DIATOM_LOG_DEBUG, "key %d -> %d", ev[i].code, ev[i].value);
			for (k = 0; k < sizeof keymap / sizeof keymap[0]; k++) {
				if (keymap[k].code != ev[i].code) continue;
				if (ev[i].value) g_buttons |=  DIATOM_BIT(keymap[k].btn);
				else             g_buttons &= ~DIATOM_BIT(keymap[k].btn);
			}
		}
	}

	/* The volume keys are the port's and stop here: never reported upward,
	 * never sent to a core. With MENU held they are brightness instead, the
	 * Pixel 2's only way to it (ADR-0037); the host sees the level move and
	 * treats MENU's release as the end of a chord. Presses only - holding a
	 * key does not repeat, as on the Brick. */
	while (g_keys_fd >= 0 && (n = read(g_keys_fd, ev, sizeof ev)) > 0) {
		for (i = 0; i < (size_t)n / sizeof ev[0]; i++) {
			int dir;

			if (ev[i].type != EV_KEY || ev[i].value != 1) continue;
			if      (ev[i].code == KEY_VOLUMEUP)   dir = +1;
			else if (ev[i].code == KEY_VOLUMEDOWN) dir = -1;
			else continue;
			if (g_input_debug)
				port_logf(DIATOM_LOG_DEBUG, "level key %d", ev[i].code);
			if (g_buttons & DIATOM_BIT(DIATOM_BTN_MENU)) bright_nudge(dir);
			else                                          gain_nudge(dir);
		}
	}
}

uint32_t diatom_port_input_state(void) { return g_buttons; }

/* ---------- levels ---------------------------------------------------------- */

/* ADR-0020's rescale: round-to-nearest, endpoints exact. */
static int rescale(int index, int from, int to)
{
	if (from <= 1 || to <= 1) return 0;
	if (index < 0)        index = 0;
	if (index > from - 1) index = from - 1;
	return (index * (to - 1) + (from - 1) / 2) / (from - 1);
}

void diatom_port_level_invalidate(void)
{
	g_level    = -1;
	g_bright   = -1;
	g_jack_was = -1;
}

bool diatom_port_level_get(diatom_level_kind kind, int *index, int *count)
{
	switch (kind) {
	case DIATOM_LEVEL_VOLUME:
		if (g_mixer_fd < 0) return false;
		gain_ensure();
		if (g_level < 0) return false;
		*index = g_level;
		*count = GAIN_LEVELS + 1;
		return true;
	case DIATOM_LEVEL_BRIGHTNESS:
		bright_ensure();
		if (g_bright < 0) return false;
		*index = g_bright;
		*count = BRIGHT_LEVELS + 1;
		return true;
	default:
		return false;
	}
}

bool diatom_port_level_set(diatom_level_kind kind, int index, int count)
{
	switch (kind) {
	case DIATOM_LEVEL_VOLUME:
		if (g_mixer_fd < 0) return false;
		g_level = rescale(index, count, GAIN_LEVELS + 1);
		gain_apply();
		return true;
	case DIATOM_LEVEL_BRIGHTNESS:
		if (backlight_read() < 0) return false;
		g_bright = rescale(index, count, BRIGHT_LEVELS + 1);
		backlight_write(bright_ladder[g_bright]);
		return true;
	default:
		return false;
	}
}

/* No hardware mute on this device, but the launcher may still hold one
 * (ADR-0031): while it does, the level never comes back on. */
void diatom_port_mute_set(bool on)
{
	if (on == g_muted) return;
	g_muted = on;
	if (g_mixer_fd >= 0 && g_level >= 0) gain_apply();
}

bool diatom_port_mute_get(void) { return g_muted; }

/* ---------- lifecycle ------------------------------------------------------- */

/* The connected connector, the CRTC driving it and its mode. The panel is the
 * only output; its preferred mode is its only mode (480x640, read with
 * modetest 2026-10-01). */
static bool kms_find(void)
{
	drmModeRes *res = drmModeGetResources(g_drm_fd);
	drmModeConnector *conn = NULL;
	drmModeEncoder *enc = NULL;
	int i;

	if (!res) return false;
	for (i = 0; i < res->count_connectors && !conn; i++) {
		drmModeConnector *c = drmModeGetConnector(g_drm_fd, res->connectors[i]);
		if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) conn = c;
		else drmModeFreeConnector(c);
	}
	if (!conn) { drmModeFreeResources(res); return false; }

	g_connector_id = conn->connector_id;
	g_mode = conn->modes[0];
	for (i = 0; i < conn->count_modes; i++)
		if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) { g_mode = conn->modes[i]; break; }

	enc = drmModeGetEncoder(g_drm_fd, conn->encoder_id ? conn->encoder_id : conn->encoders[0]);
	if (enc) {
		if (enc->crtc_id) {
			g_crtc_id = enc->crtc_id;
		} else {
			for (i = 0; i < res->count_crtcs; i++)
				if (enc->possible_crtcs & (1u << i)) { g_crtc_id = res->crtcs[i]; break; }
		}
	}
	for (i = 0; i < res->count_crtcs; i++)
		if (res->crtcs[i] == g_crtc_id) g_crtc_pipe = i;

	drmModeFreeEncoder(enc);
	drmModeFreeConnector(conn);
	drmModeFreeResources(res);
	return g_crtc_id != 0;
}

bool diatom_port_init(diatom_port_caps *out)
{
	g_input_debug   = getenv("DIATOM_INPUT_DEBUG") != NULL;
	g_present_debug = getenv("DIATOM_PRESENT_DEBUG") != NULL;

	g_drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (g_drm_fd < 0) { perror("open /dev/dri/card0"); return false; }
	/* Opening the device while nobody holds master makes us master, and a
	 * resident Diatom must not hold the display while the launcher draws
	 * (ADR-0036). Taken back at the first present. */
	drmDropMaster(g_drm_fd);

	if (!kms_find()) {
		diatom_port_log(DIATOM_LOG_ERROR, "pixel2: no connected panel");
		return false;
	}
	g_panel_w = g_mode.hdisplay;
	g_panel_h = g_mode.vdisplay;
	/* Portrait panel, landscape surface (ADR-0007). */
	g_turned    = g_panel_h > g_panel_w;
	g_surface_w = g_turned ? g_panel_h : g_panel_w;
	g_surface_h = g_turned ? g_panel_w : g_panel_h;

	g_gbm = gbm_create_device(g_drm_fd);
	if (!g_gbm) { diatom_port_log(DIATOM_LOG_ERROR, "pixel2: no GBM device"); return false; }

	if (pthread_create(&g_thread, NULL, present_thread, NULL) != 0) {
		diatom_port_log(DIATOM_LOG_ERROR, "pixel2: no presenting thread");
		return false;
	}
	pthread_mutex_lock(&g_mx);
	while (g_thread_ready == 0) pthread_cond_wait(&g_idle, &g_mx);
	pthread_mutex_unlock(&g_mx);
	if (g_thread_ready < 0) {
		pthread_join(g_thread, NULL);
		return false;
	}
	g_thread_running = true;

	g_mixer_fd = open("/dev/snd/controlC0", O_RDWR | O_CLOEXEC);
	jack_open();
	input_open();

	/* Not fatal, as on the Brick: silence beats no emulator at all, and
	 * capacity 0 is how the silence is reported (it turns rate control off). */
	if (!audio_open(NULL))
		port_logf(DIATOM_LOG_WARN, "audio unavailable (%s); continuing without sound",
		          SDL_GetError());

	port_logf(DIATOM_LOG_INFO,
	          "pixel2: panel %dx%d%s, surface %dx%d, crtc %u, audio %d Hz, pad %s",
	          g_panel_w, g_panel_h, g_turned ? " (turned)" : "",
	          g_surface_w, g_surface_h, g_crtc_id, AUDIO_RATE,
	          g_pad_fd >= 0 ? "yes" : "none");

	out->surface_w           = g_surface_w;
	out->surface_h           = g_surface_h;
	out->audio_rate          = AUDIO_RATE;
	out->audio_buffer_frames = g_audio ? AUDIO_BUFFER_FRAMES : 0;
	out->present_blocks      = false;
	return true;
}

void diatom_port_shutdown(void)
{
	int i;

	if (g_thread_running) {
		drain();
		pthread_mutex_lock(&g_mx);
		g_thread_stop = true;
		pthread_cond_signal(&g_cv);
		pthread_mutex_unlock(&g_mx);
		pthread_join(g_thread, NULL);
		g_thread_running = false;
	}
	give_display();
	for (i = 0; i < MAILBOX_FRAMES; i++) free(g_frames[i].pixels);
	/* The level and the brightness are user settings and stay where they are,
	 * as on the Brick. */
	if (g_mixer_fd >= 0) { close(g_mixer_fd); g_mixer_fd = -1; }
	if (g_pad_fd >= 0)   close(g_pad_fd);
	if (g_keys_fd >= 0)  close(g_keys_fd);
	if (g_jack_fd >= 0)  close(g_jack_fd);
	if (g_audio)         SDL_CloseAudioDevice(g_audio);
	SDL_Quit();
	/* The GBM surface and device go with the process: the buffer on glass
	 * stays allocated until then (ADR-0036). */
	if (g_drm_fd >= 0)   close(g_drm_fd);
}

bool diatom_port_should_quit(void) { return false; }

bool diatom_port_capture(const char *path)
{
	bool ok;

	if (!g_thread_running || !path) return false;
	drain();
	pthread_mutex_lock(&g_mx);
	g_capture_path = path;
	pthread_cond_signal(&g_cv);
	while (g_capture_path) pthread_cond_wait(&g_idle, &g_mx);
	ok = g_capture_ok;
	pthread_mutex_unlock(&g_mx);
	return ok;
}

uint64_t diatom_port_now_us(void)
{
	return diatom_ticks_to_us(SDL_GetPerformanceCounter(),
	                          SDL_GetPerformanceFrequency());
}

void diatom_port_log(diatom_log_level lvl, const char *msg)
{
	static const char *tag[] = { "debug", "info", "warn", "error" };
	fprintf(stderr, "[%s] %s\n", tag[lvl], msg);
}
