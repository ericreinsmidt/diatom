/* Diatom port interface - ADR-0007.
 *
 * THIS HEADER MUST NOT INCLUDE libretro.h, AND NEITHER MAY ANY PORT.
 * That is the mechanical test for whether the seam holds: a port that needs a
 * core to build cannot be developed on the desktop, which destroys the
 * iteration loop the desktop backend exists to provide.
 *
 * The port deals in pixels, samples, buttons and time. Nothing else. If a
 * function name here grows a domain noun - game, save, core, menu - it is in
 * the wrong layer.
 *
 * Port selection is compile-time: one binary per device, no plugin mechanism.
 * Nobody swaps a device backend at runtime on a handheld.
 */
#ifndef DIATOM_PORT_H
#define DIATOM_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Cores may emit 0RGB1555 too; Diatom refuses it. Measured 2026-08-23: all six
 * cores tested chose RGB565 when offered both. The XRGB8888 path is accepted
 * because it costs nothing, but it is currently untested - see the spike. */
typedef enum {
	DIATOM_PIX_RGB565,
	DIATOM_PIX_XRGB8888
} diatom_pixfmt;

typedef enum {
	DIATOM_LOG_DEBUG,
	DIATOM_LOG_INFO,
	DIATOM_LOG_WARN,
	DIATOM_LOG_ERROR
} diatom_log_level;

/* How to sample when the destination rect is not an exact integer multiple of
 * the source. Choosing is policy (host); sampling is hardware (port), the same
 * split ADR-0007 applies to the rect itself.
 *
 * SHARP is sharp-bilinear: interpolate only across the one destination pixel
 * that straddles a source-pixel boundary, leaving every interior pixel exact.
 * At an integer factor it is identical to NEAREST by construction.
 *
 * Passed per frame rather than set once, so the port holds no policy state and
 * cannot be asked to present before it has been told how. */
typedef enum {
	DIATOM_FILTER_NEAREST,
	DIATOM_FILTER_SHARP
} diatom_filter;

typedef struct { int x, y, w, h; } diatom_rect;

typedef struct {
	/* Logical and ALWAYS landscape. A port whose panel is physically rotated
	 * - the Miniloong's framebuffer is 720x960 portrait - hides that here.
	 * Nothing above the port may learn the panel's true orientation. */
	int  surface_w, surface_h;

	int  audio_rate;            /* what the device actually runs at */
	int  audio_buffer_frames;   /* capacity; DRC needs this, not just queued */
	bool present_blocks;        /* does present() wait for vblank? */
} diatom_port_caps;

/* Canonical Diatom buttons. Digital only - ADR-0003, no analog axes anywhere.
 * Bit positions within diatom_port_input_state(). Deliberately NOT libretro's
 * RETRO_DEVICE_ID_JOYPAD_*: the near-identity mapping is what keeps libretro.h
 * out of the port. Device quirks - the Brick reporting its front keys as
 * L3/R3, say - are resolved here and never travel upward. */
enum {
	DIATOM_BTN_UP = 0, DIATOM_BTN_DOWN, DIATOM_BTN_LEFT, DIATOM_BTN_RIGHT,
	DIATOM_BTN_A, DIATOM_BTN_B, DIATOM_BTN_X, DIATOM_BTN_Y,
	DIATOM_BTN_L1, DIATOM_BTN_R1, DIATOM_BTN_L2, DIATOM_BTN_R2,
	DIATOM_BTN_SELECT, DIATOM_BTN_START,
	/* Diatom's own key: never forwarded to a core. The port REPORTS it like
	 * any other button and does not act on it - what MENU means is host
	 * policy, and it differs by mode. Standalone it ends the session; under
	 * the launcher protocol it hands the display over for a menu. A port that
	 * decided this itself would make that impossible. */
	DIATOM_BTN_MENU,
	DIATOM_BTN_COUNT
};
#define DIATOM_BIT(b) (1u << (b))

bool diatom_port_init(diatom_port_caps *out);
void diatom_port_shutdown(void);

/* Called AFTER retro_run returns, never from inside the core's video callback.
 * The host owns `src` and has already computed `dst`; the port blits.
 * src == NULL means "repeat the previous frame" (the core signalled a dupe).
 *
 * `dst` may extend past the surface: a fill or overscale mode deliberately
 * crops. Ports clip; they never refuse the frame.
 *
 * Seven parameters is at the edge of reasonable. If this list grows again it
 * wants a struct, not an eighth argument. */
void diatom_port_present(const void *src, int w, int h, size_t pitch,
                         diatom_pixfmt fmt, diatom_rect dst,
                         diatom_filter filter);

/* Interleaved stereo S16 at caps.audio_rate. NEVER blocks; drops on overflow.
 * A blocking write is a legitimate sync strategy but is incompatible with
 * dynamic rate control, which the measured spread of core rates makes
 * mandatory: 32040 / 32768 / 44100 / 48000 / 65536 Hz across six cores. */
void   diatom_port_audio_write(const int16_t *frames, size_t n);
size_t diatom_port_audio_queued(void);

void     diatom_port_input_poll(void);
uint32_t diatom_port_input_state(void);

/* True once the port's surface has gone away - a closed window on desktop.
 * Not anticipated by ADR-0007; surfaced during implementation. It concerns the
 * port's own viability, not anything about games, so it belongs here. */
bool diatom_port_should_quit(void);

/* Write what was last presented. Pixels and a path, no domain nouns, and both
 * backends want it - on desktop to see what happened, on device because a
 * screenshot is otherwise unobtainable. Also not in ADR-0007: that interface
 * specified ten functions and implementation has made it twelve within a day,
 * which is worth noticing even though both additions look justified. */
bool diatom_port_capture(const char *path);

uint64_t diatom_port_now_us(void);
void     diatom_port_log(diatom_log_level lvl, const char *msg);

#endif /* DIATOM_PORT_H */
