/* Desktop port - SDL2.
 *
 * Built FIRST, before any device backend, and that ordering is deliberate: an
 * interface with only one implementation behind it grows that implementation's
 * assumptions no matter how carefully it is written. Two real backends is what
 * makes the seam honest.
 *
 * This file does not include libretro.h and must never need to. If it ever
 * does, the boundary is drawn in the wrong place.
 */
/* <SDL.h>, not <SDL2/SDL.h>: sdl2-config and pkg-config both put the SDL2
 * directory on the include path, and this is the form SDL documents. */
#include <SDL.h>
#include <stdio.h>
#include <string.h>

#include "diatom_port.h"

#define WINDOW_W 960
#define WINDOW_H 720
#define AUDIO_RATE 48000
/* Capacity in FRAMES (one frame = two int16 samples). 4096 at 48kHz is ~85ms,
 * with rate control aiming to hold it near half that. */
#define AUDIO_BUFFER_FRAMES 4096
#define AUDIO_FRAME_BYTES   (2 * (int)sizeof(int16_t))

static SDL_Window   *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture  *g_texture;
static int           g_tex_w, g_tex_h;
static Uint32        g_tex_fmt;
static SDL_AudioDeviceID g_audio;
static bool          g_quit;
static uint32_t      g_buttons;

bool diatom_port_init(diatom_port_caps *out)
{
	SDL_AudioSpec want, have;

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return false;
	}

	g_window = SDL_CreateWindow("diatom",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		WINDOW_W, WINDOW_H, SDL_WINDOW_SHOWN);
	if (!g_window) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return false; }

	g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
	if (!g_renderer) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return false; }
	SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);

	/* Vsync OFF, deliberately.
	 *
	 * No console runs at the panel's rate - measured 50.0070, 59.7275, 59.8200,
	 * 60.0000 across six cores. Blocking on a 60Hz vblank while trying to hold
	 * 59.7275 leaves 0.07ms of slack per frame, so any jitter misses a vblank
	 * and costs a whole 16.67ms. That measured as a consistent 1.4% deficit.
	 *
	 * The host paces against a monotonic clock instead, and audio drift is
	 * absorbed by rate control rather than by hoping the panel agrees. */
	if (SDL_RenderSetVSync(g_renderer, 0) != 0)
		fprintf(stderr, "note: could not disable vsync: %s\n", SDL_GetError());

	SDL_memset(&want, 0, sizeof want);
	want.freq     = AUDIO_RATE;
	want.format   = AUDIO_S16SYS;
	want.channels = 2;
	want.samples  = 1024;
	g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (!g_audio) { fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError()); return false; }
	SDL_PauseAudioDevice(g_audio, 0);

	out->surface_w          = WINDOW_W;
	out->surface_h          = WINDOW_H;
	out->audio_rate         = have.freq;
	out->audio_buffer_frames = AUDIO_BUFFER_FRAMES;
	out->present_blocks     = false;
	return true;
}

void diatom_port_shutdown(void)
{
	if (g_audio)    SDL_CloseAudioDevice(g_audio);
	if (g_texture)  SDL_DestroyTexture(g_texture);
	if (g_renderer) SDL_DestroyRenderer(g_renderer);
	if (g_window)   SDL_DestroyWindow(g_window);
	SDL_Quit();
}

static bool ensure_texture(int w, int h, diatom_pixfmt fmt)
{
	Uint32 sdlfmt = (fmt == DIATOM_PIX_RGB565)
	              ? SDL_PIXELFORMAT_RGB565
	              : SDL_PIXELFORMAT_ARGB8888;

	if (g_texture && g_tex_w == w && g_tex_h == h && g_tex_fmt == sdlfmt)
		return true;

	if (g_texture) SDL_DestroyTexture(g_texture);
	g_texture = SDL_CreateTexture(g_renderer, sdlfmt,
	                              SDL_TEXTUREACCESS_STREAMING, w, h);
	if (!g_texture) {
		fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
		return false;
	}
	/* Integer scaling only, so nearest is the correct filter, not a preference:
	 * linear on an exact integer factor is just a blurrier version of the same
	 * pixels. */
	SDL_SetTextureScaleMode(g_texture, SDL_ScaleModeNearest);
	g_tex_w = w; g_tex_h = h; g_tex_fmt = sdlfmt;
	return true;
}

void diatom_port_present(const void *src, int w, int h, size_t pitch,
                         diatom_pixfmt fmt, diatom_rect dst)
{
	SDL_Rect r;

	if (w > 0 && h > 0 && !ensure_texture(w, h, fmt)) return;
	if (src && g_texture) SDL_UpdateTexture(g_texture, NULL, src, (int)pitch);

	SDL_RenderClear(g_renderer);
	if (g_texture) {
		r.x = dst.x; r.y = dst.y; r.w = dst.w; r.h = dst.h;
		SDL_RenderCopy(g_renderer, g_texture, NULL, &r);
	}
	SDL_RenderPresent(g_renderer);
}

void diatom_port_audio_write(const int16_t *frames, size_t n)
{
	/* Never blocks; drops on overflow. A blocking write would pace the whole
	 * program off the audio clock, which rules out dynamic rate control. */
	if (!g_audio || !frames || !n) return;
	if (diatom_port_audio_queued() >= (size_t)AUDIO_BUFFER_FRAMES) return;
	SDL_QueueAudio(g_audio, frames, (Uint32)(n * AUDIO_FRAME_BYTES));
}

size_t diatom_port_audio_queued(void)
{
	if (!g_audio) return 0;
	return SDL_GetQueuedAudioSize(g_audio) / AUDIO_FRAME_BYTES;
}

static const struct { SDL_Scancode key; int btn; } keymap[] = {
	{ SDL_SCANCODE_UP,     DIATOM_BTN_UP     },
	{ SDL_SCANCODE_DOWN,   DIATOM_BTN_DOWN   },
	{ SDL_SCANCODE_LEFT,   DIATOM_BTN_LEFT   },
	{ SDL_SCANCODE_RIGHT,  DIATOM_BTN_RIGHT  },
	{ SDL_SCANCODE_X,      DIATOM_BTN_A      },
	{ SDL_SCANCODE_Z,      DIATOM_BTN_B      },
	{ SDL_SCANCODE_S,      DIATOM_BTN_X      },
	{ SDL_SCANCODE_A,      DIATOM_BTN_Y      },
	{ SDL_SCANCODE_Q,      DIATOM_BTN_L1     },
	{ SDL_SCANCODE_W,      DIATOM_BTN_R1     },
	{ SDL_SCANCODE_1,      DIATOM_BTN_L2     },
	{ SDL_SCANCODE_2,      DIATOM_BTN_R2     },
	{ SDL_SCANCODE_RSHIFT, DIATOM_BTN_SELECT },
	{ SDL_SCANCODE_RETURN, DIATOM_BTN_START  },
	{ SDL_SCANCODE_ESCAPE, DIATOM_BTN_MENU   },
};

void diatom_port_input_poll(void)
{
	const Uint8 *keys;
	SDL_Event ev;
	size_t i;
	uint32_t s = 0;

	while (SDL_PollEvent(&ev))
		if (ev.type == SDL_QUIT) g_quit = true;

	keys = SDL_GetKeyboardState(NULL);
	for (i = 0; i < sizeof keymap / sizeof keymap[0]; i++)
		if (keys[keymap[i].key]) s |= DIATOM_BIT(keymap[i].btn);

	/* Diatom's own key, never forwarded to a core. */
	if (s & DIATOM_BIT(DIATOM_BTN_MENU)) g_quit = true;

	g_buttons = s;
}

uint32_t diatom_port_input_state(void) { return g_buttons; }
bool     diatom_port_should_quit(void) { return g_quit; }

bool diatom_port_capture(const char *path)
{
	SDL_Surface *s;
	int w, h;
	bool ok;

	if (!g_renderer || !path) return false;
	SDL_GetRendererOutputSize(g_renderer, &w, &h);

	s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!s) return false;

	/* Read back what was actually presented, so the capture shows the real
	 * scaling and letterboxing rather than the core's raw framebuffer. */
	if (SDL_RenderReadPixels(g_renderer, NULL, SDL_PIXELFORMAT_ARGB8888,
	                         s->pixels, s->pitch) != 0) {
		SDL_FreeSurface(s);
		return false;
	}
	/* Save as plain 24-bit RGB. A 32-bit BMP carries a V4/V5 header with alpha
	 * masks that several readers - macOS ImageIO among them - refuse, and the
	 * alpha channel is meaningless here anyway. */
	{
		SDL_Surface *rgb = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_RGB24, 0);
		SDL_FreeSurface(s);
		if (!rgb) return false;
		ok = SDL_SaveBMP(rgb, path) == 0;
		SDL_FreeSurface(rgb);
	}
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
