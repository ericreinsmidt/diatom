/* Does SDL's audio path actually drive a bluealsa sink on this device?
 *
 * Diatom opens a named sink through SDL and the sink then carries nothing,
 * while aplay to the same device works perfectly. This is the smallest thing
 * that can tell those apart: no core, no game, no frontend.
 *
 *   btaudio <device>     e.g. bluealsa:DEV=AA:BB:...,PROFILE=a2dp
 *   btaudio              the default device, as a control
 *
 * It reports whether the queue DRAINS - the thing that was not happening - and
 * then times the close, which is where the frame loop hung.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Uint32 now_ms(void) { return SDL_GetTicks(); }

int main(int argc, char **argv)
{
	SDL_AudioSpec want, have;
	SDL_AudioDeviceID dev;
	const char *name = argc > 1 ? argv[1] : NULL;
	Sint16 buf[1024 * 2];
	Uint32 t0, i;

	if (name && *name) setenv("AUDIODEV", name, 1);
	else               unsetenv("AUDIODEV");

	if (SDL_Init(SDL_INIT_AUDIO) != 0) {
		printf("SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	printf("driver: %s\n", SDL_GetCurrentAudioDriver());

	SDL_memset(&want, 0, sizeof want);
	want.freq = 48000; want.format = AUDIO_S16SYS;
	want.channels = 2; want.samples = 1024;

	dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (!dev) { printf("open %s: %s\n", name ? name : "default", SDL_GetError()); return 1; }
	printf("opened %s: %d Hz, %d ch, %d frames/period, %u bytes\n",
	       name ? name : "the default device",
	       have.freq, have.channels, have.samples, have.size);

	SDL_PauseAudioDevice(dev, 0);

	/* Two seconds of silence, a period at a time, the way the frontend does.
	 * If the device drains, queued stays near zero. If it does not, it climbs
	 * and stays climbed - which is the reported failure. */
	memset(buf, 0, sizeof buf);
	t0 = now_ms();
	for (i = 0; i < 94; i++) {          /* 94 * 1024 frames ~= 2.0 s */
		SDL_QueueAudio(dev, buf, sizeof buf);
		SDL_Delay(21);                  /* one period of wall clock */
		if (i % 16 == 0)
			printf("  t=%4u ms  queued=%u bytes\n",
			       now_ms() - t0, SDL_GetQueuedAudioSize(dev));
	}
	printf("after 2s: queued=%u bytes  (near 0 = draining)\n",
	       SDL_GetQueuedAudioSize(dev));

	/* And the close, which is where Diatom's main thread stopped forever. */
	printf("closing...\n");
	fflush(stdout);
	t0 = now_ms();
	SDL_CloseAudioDevice(dev);
	printf("close returned after %u ms\n", now_ms() - t0);

	SDL_Quit();
	return 0;
}
