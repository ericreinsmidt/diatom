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
 * src == NULL means "repeat the previous frame" (the core signaled a dupe).
 *
 * `dst` may extend past the surface: a fill or overscale mode deliberately
 * crops. Ports clip; they never refuse the frame.
 *
 * Seven parameters is at the edge of reasonable. If this list grows again it
 * wants a struct, not an eighth argument. */
void diatom_port_present(const void *src, int w, int h, size_t pitch,
                         diatom_pixfmt fmt, diatom_rect dst,
                         diatom_filter filter);

/* An image composited over the presented frame for a while, in SCREEN space -
 * the same job the port already does for its own level bar, which is why it
 * lives here rather than in the host: the OSD is drawn after scaling, and a
 * host that wanted to draw it would have to copy and scale the frame itself.
 *
 * Pixels and a duration. The port is not told what the image means, and it
 * must not care: `overlay` is a pixel word, and a parameter named for an
 * achievement or a message would be this header's own rule being broken.
 *
 * `bgra` is 8-bit BGRA, straight (not premultiplied), `w * 4` bytes per row.
 * THE POINTER IS BORROWED, not copied: the host guarantees it stays valid and
 * unchanged until the next call or until the duration elapses. That keeps the
 * buffer in one place instead of one per port, which matters on a device where
 * a notice is 150KB and the whole process is meant to hold 8MB.
 *
 * ms == 0, or bgra == NULL, clears it. An image larger than the surface is
 * refused rather than clipped: something drawn half off the screen is a bug
 * that looks like a design choice. */
void diatom_port_overlay(const uint8_t *bgra, int w, int h, unsigned ms);

/* Stop presenting, without tearing down. Returns when nothing is pending and
 * nothing is in flight, so a SECOND presenter may take the display safely.
 * The port stays initialized and must serve the next diatom_port_present.
 *
 * This exists because presenting lifetime and process lifetime came apart.
 * init/shutdown bracket the process and present() is per frame; nothing
 * bracketed one GAME's presenting, which was fine while a frontend ran one
 * game and exited. ADR-0008 made the process long-lived and ADR-0016 handed
 * the display to the launcher per game, and the seam was never grown to
 * match - so `EXIT` and `PAUSED` were sent while this port was still panning,
 * and for a few milliseconds two processes drove the same framebuffer.
 * Measured from the launcher side as a boundary sweeping down the panel
 * across several frames: a tear, not a composite.
 *
 * `park` says what to LEAVE on glass, because the port cannot know and the
 * answer differs by handover:
 *
 *   KEEP  - the last frame. Right for a pause, where the launcher draws its
 *           menu over the frame the player stopped on and wants continuity.
 *   BLANK - opaque black. Right for an exit, where the launcher's first act
 *           is to fade the shelf up from black, so black is the state it is
 *           about to assume anyway.
 *
 * Getting that backwards is visible. Parking KEEP at exit replaces whatever
 * was on screen - the launcher's own menu, if the player quit from it - with
 * a bare game frame for the few tens of milliseconds before the fade starts,
 * which reads as a flash. It is not a tear: it is the wrong picture, shown
 * cleanly.
 *
 * Not diatom_port_shutdown - that joins the flip thread, and the thread has
 * to survive to serve the next game. */
typedef enum {
	DIATOM_PARK_KEEP,
	DIATOM_PARK_BLANK
} diatom_park;

void diatom_port_present_stop(diatom_park park);

/* Interleaved stereo S16 at caps.audio_rate. NEVER blocks; drops on overflow.
 * A blocking write is a legitimate sync strategy but is incompatible with
 * dynamic rate control, which the measured spread of core rates makes
 * mandatory: 32040 / 32768 / 44100 / 48000 / 65536 Hz across six cores.
 *
 * RETURNS the number of frames accepted, which may be fewer than `n` and may
 * be zero. Returning void made "drops on overflow" unobservable: the host
 * could not tell a dropped frame from a written one, so the only evidence of
 * trouble was the queue depth - and the queue was allowed to exceed the
 * capacity reported in caps, which hid it there too. A port must never accept
 * more than caps.audio_buffer_frames; partial acceptance is how it says no. */
size_t diatom_port_audio_write(const int16_t *frames, size_t n);
size_t diatom_port_audio_queued(void);

/* WHERE the sound goes, named the way this port's audio system names a device -
 * an ALSA device string on the Brick, an SDL device name on the desktop.
 *
 * ADR-0029: the host says WHICH, the port knows HOW. The string crosses here
 * exactly as a core path does, and the port never learns what kind of thing it
 * names. "bluetooth" is not a word this interface knows, for the same reason
 * Diatom keeps no core list - which output to use is the host application's
 * decision, and a port that answered it would be holding a device list.
 *
 * NULL or empty is the default device.
 *
 * FALLS BACK rather than failing. A named device that will not open leaves the
 * port on the default and still running, because a sink that can be set while a
 * game runs can fail while a game runs, and ending the game is never the better
 * answer. Returns true when it opened what was asked for and false when it fell
 * back, filling `actual` either way - the host reports where the sound IS, not
 * where it asked for it to be.
 *
 * Reopening does not change caps.audio_rate: the port asks SDL for a fixed rate
 * and lets it convert, so a sink that runs at another rate is the port's problem
 * and never the resampler's. */
bool diatom_port_audio_set(const char *name, char *actual, size_t cap);

/* Where the sound actually is now.
 *
 * Polled, like the level pair below and for the same reason: the port must not
 * know the launcher protocol exists (ADR-0007), so it cannot announce anything.
 * A sink that dies under the port - a headset switched off, or walking out of
 * range - is noticed here, because the port falls back on its own and the answer
 * changes. The host reads it beside the input bitfield it already polls. */
void diatom_port_audio_get(char *out, size_t cap);

void     diatom_port_input_poll(void);
uint32_t diatom_port_input_state(void);

/* Levels the user can change with the device's own keys while a game runs -
 * volume and brightness on the Brick, nothing at all on the desktop.
 *
 * Polled rather than pushed. The port must not know the launcher protocol
 * exists (ADR-0007), so it cannot report anything itself; the host reads these
 * alongside the input bitfield it already polls every frame and emits a
 * protocol event when a value moves. No callback into the host, no work in the
 * port's key handler beyond what it already does.
 *
 * `index` is 0-based over `0 .. *count - 1`, and `*count` is the number of
 * distinct positions rather than a maximum index - the two differ by one, and
 * ADR-0020 pins it here because a shared scale that is off by one produces a
 * silent disagreement instead of an error. Raw device units (mixer registers,
 * backlight duty) never leave the port.
 *
 * Returns false for a kind this port has no control over. */
typedef enum {
	DIATOM_LEVEL_VOLUME = 0,
	DIATOM_LEVEL_BRIGHTNESS,
	DIATOM_LEVEL_COUNT
} diatom_level_kind;

/* Forget everything the port remembers about the output state, and re-read the
 * hardware on the next get.
 *
 * Needed because Diatom is RESIDENT: the port caches its level to avoid an
 * ioctl per frame, and the launcher owns levels whenever Diatom is not
 * presenting (ADR-0020). So between games, and across a menu, the value in the
 * port can be overwritten underneath it. Without this the first press after a
 * handover steps from a level nobody is at.
 *
 * That includes the LAST HEADPHONE JACK STATE the port acted on, and forgetting
 * it is not an extra: a port re-maps the level between the speaker and
 * headphone windows on a transition IT observes, and it observes none while the
 * launcher is driving. A cable pulled out between games therefore leaves a
 * headphone-window value in the register that neither side re-maps, because
 * each sees its own remembered state already agreeing with the hardware. On
 * this device that reads as a working speaker at almost no volume. Measured
 * 2026-09-05: the register held 29 with a speaker window whose quiet end is 39.
 *
 * So this is called at every handover, and the next poll re-applies rather than
 * trusting a memory formed while something else was driving. */
void diatom_port_level_invalidate(void);

bool diatom_port_level_get(diatom_level_kind kind, int *index, int *count);
bool diatom_port_level_set(diatom_level_kind kind, int index, int count);

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
