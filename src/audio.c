/* Resampling, provisionally.
 *
 * Cores emit whatever rate their hardware ran at; the device runs at whatever
 * it runs at. Measured across six cores: 32040, 32768, 44100, 44100, 48000,
 * 65536 Hz. mGBA's 65536 is above any device rate, so this resamples in both
 * directions and never by a tidy ratio.
 *
 * This is LINEAR INTERPOLATION and it is a placeholder. Register §6 has the
 * real design open: dynamic rate control, nudging the ratio to keep the port's
 * buffer near half full. That is not optional here — no console runs at 60Hz
 * (measured: 50.0070 PAL, 59.7275, 59.8200, 60.0000), so a fixed ratio drifts
 * against the panel forever and eventually underruns or overflows.
 *
 * Good enough to hear a game. Not good enough to ship.
 */
#include <stdlib.h>
#include <string.h>

#include "diatom.h"

#define OUT_CHUNK 2048

static double   g_ratio = 1.0;      /* src frames consumed per dst frame */
static double   g_phase;
static int16_t  g_prev[2];
static bool     g_have_prev;

void diatom_audio_configure(double src_rate, int dst_rate)
{
	if (src_rate <= 0.0 || dst_rate <= 0) { g_ratio = 1.0; return; }
	g_ratio     = src_rate / (double)dst_rate;
	g_phase     = 0.0;
	g_have_prev = false;
}

size_t diatom_audio_push(const int16_t *in, size_t frames)
{
	int16_t out[OUT_CHUNK * 2];
	size_t produced = 0, i = 0;

	if (!in || !frames) return 0;

	/* Pass-through when the rates already match — mGBA answering
	 * GET_TARGET_SAMPLE_RATE puts us here, which is the point of implementing
	 * that command. */
	if (g_ratio > 0.999 && g_ratio < 1.001) {
		diatom_port_audio_write(in, frames);
		return frames;
	}

	while (i < frames) {
		while (g_phase >= 1.0 && i < frames) {
			g_prev[0] = in[i * 2];
			g_prev[1] = in[i * 2 + 1];
			g_have_prev = true;
			g_phase -= 1.0;
			i++;
		}
		if (i >= frames) break;

		{
			const int16_t *cur = &in[i * 2];
			double t = g_phase;
			int ch;
			for (ch = 0; ch < 2; ch++) {
				double a = g_have_prev ? g_prev[ch] : cur[ch];
				double v = a + (cur[ch] - a) * t;
				out[produced * 2 + ch] = (int16_t)v;
			}
		}
		produced++;
		g_phase += g_ratio;

		if (produced == OUT_CHUNK) {
			diatom_port_audio_write(out, produced);
			produced = 0;
		}
	}

	if (produced) diatom_port_audio_write(out, produced);
	return produced;
}
