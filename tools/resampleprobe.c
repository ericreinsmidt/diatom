/* What does the resampler actually do to a signal it cannot argue with?
 *
 * Game audio is a bad test bench. `hfprobe` measured "energy above 10 kHz" on
 * Contra and scored LINEAR INTERPOLATION as the better filter - because linear
 * interpolation has 2.5 dB of droop up there, and losing treble looks identical
 * to not adding artefacts when all you have is one band ratio. A resampler has
 * to be judged on a signal whose spectrum is known exactly.
 *
 * So: push a pure tone through the real `src/audio.c` at a chosen rate ratio,
 * write what comes out, and let `tools/hfprobe.py --spurs` find everything that
 * is not the tone. Anything else IS the filter, because the input had one
 * component and nothing else.
 *
 *   resampleprobe <src_rate> <dst_rate> <tone_hz> <seconds> <out.raw>
 *
 * Links src/audio.c directly and stubs the two port calls it makes, so it
 * measures the shipping code rather than a copy of it that can drift.
 *
 * An instrument. See tools/README.md.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diatom.h"

static FILE  *g_out;
static size_t g_queued;

/* The port, reduced to what audio.c touches. Reporting a steady half-full queue
 * keeps rate control at rest, so the ratio stays at its nominal value and the
 * measurement is of the FILTER rather than of the controller chasing a level. */
size_t diatom_port_audio_write(const int16_t *frames, size_t n)
{
	fwrite(frames, sizeof(int16_t) * 2, n, g_out);
	return n;
}
size_t diatom_port_audio_queued(void) { return g_queued; }

int main(int argc, char **argv)
{
	double src, tone, secs;
	int dst;
	size_t total, done = 0;
	int16_t block[1024 * 2];

	if (argc != 6) {
		fprintf(stderr, "usage: resampleprobe <src_rate> <dst_rate> <tone_hz>"
		                " <seconds> <out.raw>\n");
		return 1;
	}
	src  = atof(argv[1]);
	dst  = atoi(argv[2]);
	tone = atof(argv[3]);
	secs = atof(argv[4]);
	g_out = fopen(argv[5], "wb");
	if (!g_out) { perror("open"); return 2; }

	g_queued = 2048;                       /* half of the usual 4096 capacity */
	diatom_audio_configure(src, dst, 4096);

	total = (size_t)(src * secs);
	while (done < total) {
		size_t n = total - done, i;
		if (n > 1024) n = 1024;

		for (i = 0; i < n; i++) {
			/* -6 dBFS: high enough to sit well above the noise floor, low
			 * enough that a windowed sinc's overshoot on a full-scale tone
			 * cannot clip and manufacture harmonics of its own. */
			double v = 16384.0 * sin(2.0 * M_PI * tone * (double)(done + i) / src);
			block[i * 2] = block[i * 2 + 1] = (int16_t)v;
		}
		diatom_audio_push(block, n);
		done += n;
	}
	fclose(g_out);
	return 0;
}
