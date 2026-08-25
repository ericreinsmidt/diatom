/* SPIKE: stand-in for PlayOS. Presents through SDL2's mali/EGL driver, the
 * same path the launcher uses, so a handoff can be exercised without killing
 * and respawning the real launcher.
 *
 * An instrument, not Diatom code. Used by the display-handoff spike. */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	SDL_Window   *w;
	SDL_Renderer *r;
	SDL_DisplayMode dm;
	int frames = argc > 1 ? atoi(argv[1]) : 60;
	int tint   = argc > 2 ? atoi(argv[2]) : 0;
	int i;
	Uint32 t0;

	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "EGL: SDL_Init FAILED: %s\n", SDL_GetError());
		return 1;
	}
	if (SDL_GetCurrentDisplayMode(0, &dm) != 0) {
		fprintf(stderr, "EGL: no display mode: %s\n", SDL_GetError());
		return 2;
	}
	w = SDL_CreateWindow("egl", 0, 0, dm.w, dm.h, SDL_WINDOW_FULLSCREEN);
	if (!w) { fprintf(stderr, "EGL: CreateWindow FAILED: %s\n", SDL_GetError()); return 3; }
	r = SDL_CreateRenderer(w, -1, 0);
	if (!r) { fprintf(stderr, "EGL: CreateRenderer FAILED: %s\n", SDL_GetError()); return 4; }

	t0 = SDL_GetTicks();
	for (i = 0; i < frames; i++) {
		SDL_SetRenderDrawColor(r, tint ? 200 : 20, tint ? 20 : 200, 60, 255);
		SDL_RenderClear(r);
		SDL_RenderPresent(r);
	}
	printf("EGL: ok, driver=%s %dx%d, %d frames in %u ms\n",
	       SDL_GetCurrentVideoDriver(), dm.w, dm.h, frames, SDL_GetTicks() - t0);

	SDL_DestroyRenderer(r);
	SDL_DestroyWindow(w);
	SDL_Quit();
	return 0;
}
