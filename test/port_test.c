/* The port's audio contract, exercised against a real port.
 *
 * ADR-0007 says the port owns opening a device and the host owns choosing one,
 * and ADR-0029 leans on three properties that nothing here used to check:
 *
 *   it falls back    a device that will not open leaves the port on the
 *                    default and STILL RUNNING, because a sink that can be set
 *                    while a game runs can fail while a game runs
 *   it is idempotent re-stating the current device does not tear down a
 *                    working stream and lose what is queued
 *   it returns       switching NEVER blocks the caller
 *
 * The last one is why this file exists. Closing an SDL audio device joins its
 * audio thread, and that thread can be stuck writing to a sink that stopped
 * draining - so the close never returns and the frame loop stops forever. That
 * shipped on 2026-09-04 and wedged the device on the 5th, with the emulation
 * still running behind a frozen picture. A hang is not a failure any assert
 * can catch, so this arms an alarm and lets the timeout be the failure.
 *
 * Runs against port/desktop.c with the dummy audio driver: no device, no
 * sound, no network. The Brick's port is the same code shape and cannot be
 * driven from here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "diatom_port.h"

static int fails;

static void ck(int cond, const char *what)
{
	printf(cond ? "  ok    %s\n" : "  FAIL  %s\n", what);
	if (!cond) fails++;
}

#define BOGUS "no such sink anywhere on this machine"

int main(void)
{
	diatom_port_caps caps;
	char at[128];
	int16_t buf[512 * 2];
	size_t queued_before, queued_after, took;
	int i;

	/* SDL must not reach a real device, a real display, or the user's ears. */
	setenv("SDL_AUDIODRIVER", "dummy", 1);
	setenv("SDL_VIDEODRIVER", "dummy", 1);
	/* The dummy video driver offers no accelerated renderer, so name the
	 * software one - the same pair test/conform.py uses. */
	setenv("SDL_RENDER_DRIVER", "software", 1);

	/* A wedged close is a hang, not a wrong answer. Twenty seconds is far
	 * beyond anything this does - the whole file runs in well under one. */
	alarm(20);

	memset(buf, 0, sizeof buf);

	if (!diatom_port_init(&caps)) {
		printf("  FAIL  the port would not initialise\n");
		return 1;
	}

	printf("caps:\n");
	ck(caps.audio_rate > 0, "a rate the resampler can convert into");
	ck(caps.audio_buffer_frames >= 0, "a capacity, even when it is zero");

	printf("where the sound is:\n");
	diatom_port_audio_get(at, sizeof at);
	ck(at[0] == '\0', "starts on the default device, reported as empty");

	printf("a device that cannot be opened:\n");
	ck(!diatom_port_audio_set(BOGUS, at, sizeof at),
	   "reports that it did not get what was asked for");
	ck(at[0] == '\0', "and says it landed on the default instead");
	ck(diatom_port_audio_write(buf, 512) > 0,
	   "audio still flows after the fallback");

	printf("re-stating the current device:\n");
	diatom_port_audio_write(buf, 512);
	queued_before = diatom_port_audio_queued();
	ck(diatom_port_audio_set("", at, sizeof at), "succeeds");
	queued_after = diatom_port_audio_queued();
	/* If it reopened, the queue would be gone. That reopen costs a fresh
	 * stream setup on a real sink and drops whatever was buffered. */
	ck(!(queued_before > 0 && queued_after == 0),
	   "does not throw away what was already queued");

	printf("the queue respects the capacity it advertises:\n");
	for (i = 0; i < 64; i++) diatom_port_audio_write(buf, 512);
	ck(caps.audio_buffer_frames == 0 ||
	   diatom_port_audio_queued() <= (size_t)caps.audio_buffer_frames,
	   "never holds more than caps.audio_buffer_frames");
	took = diatom_port_audio_write(buf, 512);
	ck(took == 0 || diatom_port_audio_queued() <= (size_t)caps.audio_buffer_frames,
	   "a full queue refuses rather than overflowing");

	/* The one that matters. If a close ever blocks again, this does not fail -
	 * it stops, and the alarm above turns that into a failure with a name. */
	printf("switching, repeatedly, must always return:\n");
	for (i = 0; i < 20; i++)
		diatom_port_audio_set(i % 2 ? BOGUS : "", at, sizeof at);
	ck(1, "20 switches completed without hanging");
	ck(diatom_port_audio_write(buf, 512) > 0, "and audio still flows after");

	diatom_port_shutdown();

	if (fails) { printf("\n%d port check(s) failed\n", fails); return 1; }
	printf("\nok: the port keeps its side of the bargain\n");
	return 0;
}
