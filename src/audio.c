/* Resampling, provisionally.
 *
 * Cores emit whatever rate their hardware ran at; the device runs at whatever
 * it runs at. Measured across six cores: 32040, 32768, 44100, 44100, 48000,
 * 65536 Hz. mGBA's 65536 is above any device rate, so this resamples in both
 * directions and never by a tidy ratio.
 *
 * This is LINEAR INTERPOLATION and it is a placeholder. Register §6 has the
 * real design open: dynamic rate control, nudging the ratio to keep the port's
 * buffer near half full. That is not optional here - no console runs at 60Hz
 * (measured: 50.0070 PAL, 59.7275, 59.8200, 60.0000), so a fixed ratio drifts
 * against the panel forever and eventually underruns or overflows.
 *
 * Good enough to hear a game. Not good enough to ship.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "diatom.h"

#define OUT_CHUNK 2048

/* How far the resample ratio may be nudged. 0.5% is the usual figure: enough to
 * absorb clock drift, small enough that the pitch shift is inaudible. */
#define MAX_DEVIATION 0.005
#define P_GAIN 1.0
#define I_GAIN 0.002     /* per frame; deliberately slow, ~8s to full authority */

static double   g_base_ratio = 1.0; /* src frames per dst frame, nominal */
static double   g_ratio      = 1.0; /* nominal, adjusted by rate control */
static double   g_phase;
static int16_t  g_prev[2];
static bool     g_have_prev;
static int      g_capacity;
static double   g_integral;
static uint64_t g_dropped;      /* frames the port would not take */

/* Every write goes through here so a refusal is counted exactly once. Silent
 * drops are how the overshoot went unnoticed: the queue depth was the only
 * evidence, and the queue was permitted to exceed its own stated capacity. */
/* Peak absolute sample seen this session, and how many were non-zero. Added
 * 2026-08-25 after two hours spent on why Diatom was inaudible while minarch
 * was fine on the same codec with byte-identical PCM parameters. Everything
 * downstream of this function is shared with the working case, so the only
 * thing that can differ is the data - and nothing measured the data. */
static int      g_peak;
static uint64_t g_nonzero, g_total;
static double   g_sq;

static void push(const int16_t *f, size_t n)
{
	size_t i, took;

	for (i = 0; i < n * 2; i++) {
		int v = f[i] < 0 ? -f[i] : f[i];
		if (v > g_peak) g_peak = v;
		if (v) g_nonzero++;
		g_sq += (double)v * v;
	}
	g_total += (uint64_t)n * 2;

	took = diatom_port_audio_write(f, n);
	if (took < n) g_dropped += (uint64_t)(n - took);
}

/* The same measurement on the INPUT side, before any resampling, so a silent
 * output can be attributed to the core or to us without guessing. */
static int      g_in_peak;
static uint64_t g_in_nonzero, g_in_total;
static double   g_in_sq;

void diatom_audio_note_input(const int16_t *f, size_t n)
{
	size_t i;
	for (i = 0; i < n * 2; i++) {
		int v = f[i] < 0 ? -f[i] : f[i];
		if (v > g_in_peak) g_in_peak = v;
		if (v) g_in_nonzero++;
		g_in_sq += (double)v * v;
	}
	g_in_total += (uint64_t)n * 2;
}

double   diatom_audio_in_rms(void)
{ return g_in_total ? sqrt(g_in_sq / (double)g_in_total) : 0.0; }
int      diatom_audio_in_peak(void)    { return g_in_peak; }
uint64_t diatom_audio_in_nonzero(void) { return g_in_nonzero; }
uint64_t diatom_audio_in_samples(void) { return g_in_total; }

double   diatom_audio_rms(void)
{ return g_total ? sqrt(g_sq / (double)g_total) : 0.0; }
int      diatom_audio_peak(void)    { return g_peak; }
uint64_t diatom_audio_nonzero(void) { return g_nonzero; }
uint64_t diatom_audio_samples(void) { return g_total; }

uint64_t diatom_audio_dropped(void) { return g_dropped; }

void diatom_audio_configure(double src_rate, int dst_rate, int capacity_frames)
{
	if (src_rate <= 0.0 || dst_rate <= 0) { g_base_ratio = g_ratio = 1.0; return; }
	g_base_ratio = src_rate / (double)dst_rate;
	g_ratio      = g_base_ratio;
	g_phase      = 0.0;
	g_have_prev  = false;
	g_capacity   = capacity_frames;
	g_integral   = 0.0;
	g_dropped    = 0;
	g_peak       = 0;
	g_nonzero    = 0;
	g_total      = 0;
	g_in_peak    = 0;
	g_in_nonzero = 0;
	g_in_total   = 0;
	g_sq = g_in_sq = 0.0;
}

/* Fill the buffer to target before the first frame runs.
 *
 * Without this the buffer starts empty and stays there: the core produces
 * exactly one frame of audio per video frame, so production matches consumption
 * and nothing ever fills the gap. Rate control can only pull at 0.5%, which
 * takes about eight seconds to accumulate 2048 frames - and until it does, every
 * scheduling hiccup underruns. Measured before this existed: `queued min 0`.
 *
 * Priming with silence costs one buffer of latency at startup, which is the
 * latency we were going to have anyway once the buffer filled.
 */
void diatom_audio_prime(void)
{
	int16_t silence[512 * 2];
	int remaining = g_capacity / 2;

	memset(silence, 0, sizeof silence);
	while (remaining > 0) {
		int n = remaining > 512 ? 512 : remaining;
		push(silence, (size_t)n);
		remaining -= n;
	}
}

/* Dynamic rate control. Called once per frame, after the frame's audio has been
 * pushed.
 *
 * The device consumes at its own crystal's idea of 48kHz; the core produces at
 * its own idea of 32040. Nothing keeps those aligned, and a fixed ratio drifts
 * until the buffer either empties or overflows - the only question is which,
 * and how long it takes. Nudging the ratio toward keeping the buffer half full
 * closes the loop, and it is why the port must expose capacity and not just
 * occupancy (ADR-0007).
 */
void diatom_audio_sync(void)
{
	double target, delta, adjust;
	size_t queued;

	if (g_capacity <= 0) return;

	queued = diatom_port_audio_queued();
	target = g_capacity / 2.0;
	delta  = ((double)queued - target) / target;   /* -1 empty .. +1 full */

	if (delta < -1.0) delta = -1.0;
	if (delta >  1.0) delta =  1.0;

	/* Proportional plus integral.
	 *
	 * Proportional alone cannot sit on target: a persistent correction requires
	 * a persistent error, so the buffer stabilises wherever the error happens to
	 * generate the needed nudge. Measured with P only: it held around 500-770
	 * frames against a 2048 target, and dipped to 26 - a stall of 10ms from
	 * underrunning. The integral term accumulates the residual and drives the
	 * steady-state error to zero.
	 *
	 * The integral is clamped rather than left to wind up: without that, a long
	 * stall banks correction that then overshoots for just as long. */
	g_integral += delta * I_GAIN;
	if (g_integral < -1.0) g_integral = -1.0;
	if (g_integral >  1.0) g_integral =  1.0;

	adjust = delta * P_GAIN + g_integral;
	if (adjust < -1.0) adjust = -1.0;
	if (adjust >  1.0) adjust =  1.0;

	/* Buffer filling up means we are producing too fast, so consume more source
	 * per output frame - a larger ratio yields fewer output frames. */
	g_ratio = g_base_ratio * (1.0 + adjust * MAX_DEVIATION);
}

double diatom_audio_ratio_drift(void)
{
	return g_base_ratio > 0.0 ? (g_ratio / g_base_ratio) - 1.0 : 0.0;
}

/* Read frame k of the virtual stream: index 0 is the frame carried over from the
 * previous block, 1..frames are this block. That carry is what lets phase run
 * continuously across calls instead of restarting each time. */
static void tap(const int16_t *in, size_t frames, int k, double *l, double *r)
{
	const int16_t *p;
	if (k <= 0)                 p = g_prev;
	else if ((size_t)k > frames) p = &in[(frames - 1) * 2];
	else                         p = &in[(k - 1) * 2];
	*l = p[0];
	*r = p[1];
}

size_t diatom_audio_push(const int16_t *in, size_t frames)
{
	int16_t out[OUT_CHUNK * 2];
	size_t produced = 0, total = 0;

	if (!in || !frames) return 0;

	/* Pass-through when the rates already match - a core answering
	 * GET_TARGET_SAMPLE_RATE lands here, which is the point of implementing it.
	 * Rate control is skipped too; there is nothing to nudge. */
	if (g_ratio > 0.9999 && g_ratio < 1.0001) {
		push(in, frames);
		return frames;
	}

	if (!g_have_prev) {
		g_prev[0] = in[0];
		g_prev[1] = in[1];
		g_have_prev = true;
	}

	/* g_phase is the read position in the virtual stream and PERSISTS across
	 * calls. The previous implementation broke out when the block ran dry and
	 * dropped the pending output frame - about one per call, ~60/second against
	 * 48000, a 0.125% leak. Small enough to look like clock drift and big enough
	 * to drain the buffer to empty in twenty seconds, with rate control pinned at
	 * its limit the whole way. */
	while (g_phase < (double)frames) {
		int idx = (int)g_phase;
		double t = g_phase - idx;
		double al, ar, bl, br;

		tap(in, frames, idx,     &al, &ar);
		tap(in, frames, idx + 1, &bl, &br);

		out[produced * 2]     = (int16_t)(al + (bl - al) * t);
		out[produced * 2 + 1] = (int16_t)(ar + (br - ar) * t);
		produced++;
		g_phase += g_ratio;

		if (produced == OUT_CHUNK) {
			push(out, produced);
			total += produced;
			produced = 0;
		}
	}

	g_phase -= (double)frames;          /* carry the remainder forward */
	g_prev[0] = in[(frames - 1) * 2];
	g_prev[1] = in[(frames - 1) * 2 + 1];

	if (produced) { push(out, produced); total += produced; }
	return total;
}
