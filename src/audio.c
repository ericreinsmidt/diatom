/* Resampling.
 *
 * Cores emit whatever rate their hardware ran at; the device runs at whatever
 * it runs at. Measured from the pinned set - docs/reference/core-facts.md, which
 * is generated rather than transcribed: 32040 (SNES), 44100 (Sega, PC Engine),
 * 48000 (NES), 65536 (GBA) and 131072 Hz (Game Boy). The last is 2.73x the
 * device rate, so this resamples in both directions and never by a tidy ratio.
 *
 * A POLYPHASE WINDOWED-SINC FIR, 32 taps over 512 phases. It replaced linear
 * interpolation, which was a placeholder that measurement caught: on Contra,
 * 18% of 50 ms windows left the resampler with MORE energy above 10 kHz than
 * they arrived with. A lowpass cannot add high frequencies, so that energy was
 * imaging - linear interpolation attenuates the spectral images a rate change
 * creates by only ~13 dB near Nyquist, and they fold back into the audible
 * band. A 32-tap Blackman-windowed sinc puts them below -70 dB.
 *
 * Rate control rides on top: the ratio is nudged +-0.5% to keep the port's
 * buffer near half full. That is not optional here - no console runs at 60Hz
 * (measured: 50.0070 PAL, 59.7275, 59.8200, 60.0000), so a fixed ratio drifts
 * against the panel forever and eventually underruns or overflows.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "diatom.h"

#define OUT_CHUNK 2048

/* Raw taps, opt-in. Writing to /tmp on the device is tmpfs, so this is a RAM
 * copy rather than an SD write in the frame path. Both sides are captured so a
 * defect can be attributed: present in IN means the core or the game, present
 * only in OUT means us, present in neither means downstream of us. */
static FILE *g_tap_in, *g_tap_out;

void diatom_audio_tap(const char *in_path, const char *out_path)
{
	if (in_path)  g_tap_in  = fopen(in_path,  "wb");
	if (out_path) g_tap_out = fopen(out_path, "wb");
}

void diatom_audio_tap_close(void)
{
	if (g_tap_in)  { fclose(g_tap_in);  g_tap_in  = NULL; }
	if (g_tap_out) { fclose(g_tap_out); g_tap_out = NULL; }
}

/* How far the resample ratio may be nudged. 0.5% is the usual figure: enough to
 * absorb clock drift, small enough that the pitch shift is inaudible. */
#define MAX_DEVIATION 0.005
#define P_GAIN 1.0
#define I_GAIN 0.002     /* per frame; deliberately slow, ~8s to full authority */

/* The filter. 32 taps is ~70 dB of image rejection with a Blackman window,
 * against linear interpolation's ~13; 512 phases put the residual timing jitter
 * around 20 ns, far below anything audible, which is what lets the inner loop
 * pick a phase rather than interpolate between two and do twice the work.
 *
 * Both tables are static. Diatom allocates three times in its whole life
 * (§11) and that is a property worth not spending here - 98 kB of BSS costs
 * nothing on a device with 975 MB and keeps the frame loop allocation-free. */
#define TAPS    32
#define HALF    (TAPS / 2)
#define PHASES  512
#define ROLLOFF 0.92           /* cutoff, as a fraction of the lower Nyquist */
#define MAX_IN  8192           /* input frames accepted in one pass */

static void build_kernel(void);

static float    g_kern[PHASES][TAPS];
static int16_t  g_buf[MAX_IN + TAPS][2];
static size_t   g_have;             /* frames held in g_buf */
static double   g_pos;              /* read position within g_buf */

static double   g_base_ratio = 1.0; /* src frames per dst frame, nominal */
static double   g_ratio      = 1.0; /* nominal, adjusted by rate control */
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

/* QUIET, ADR-0032: the game's sound replaced by silence while the launcher has
 * something else playing, with the stream itself carrying on untouched.
 *
 * A gain walked one step a frame toward its target rather than a switch,
 * because a switch is a step in the waveform, and a step is broadband energy -
 * a click - whatever the waveform was doing. 10 ms is the usual length for a
 * ramp nobody hears and a cut nobody waits for; the ADR leaves the number to
 * the device, so it is set here and checked there.
 *
 * Applied last, after the resampler and after rate control, which therefore
 * never know: the buffer fills and drains exactly as it would have. */
#define QUIET_FADE_MS 10

static bool     g_quiet;               /* what the launcher asked for */
static float    g_gain = 1.0f;         /* where the output is, on the way there */
static float    g_gain_step = 1.0f;    /* per frame; set from the device rate */
static uint64_t g_quiet_frames;
static int16_t  g_quiet_buf[OUT_CHUNK * 2];

void diatom_audio_quiet(bool on)  { g_quiet = on; }
bool diatom_audio_quiet_get(void) { return g_quiet; }
uint64_t diatom_audio_quiet_frames(void) { return g_quiet_frames; }

/* `n` frames of `in`, scaled by the quiet gain into g_quiet_buf, the gain
 * moving one step a frame. Only called when there is something to do: at full
 * gain with nothing asked, the frames go out as they came. */
static const int16_t *quieten(const int16_t *in, size_t n)
{
	float target = g_quiet ? 0.0f : 1.0f;
	size_t i;

	g_quiet_frames += n;
	if (g_gain == 0.0f && target == 0.0f) {
		memset(g_quiet_buf, 0, n * 2 * sizeof(int16_t));
		return g_quiet_buf;
	}
	for (i = 0; i < n; i++) {
		if (g_gain > target) {
			g_gain -= g_gain_step;
			if (g_gain < target) g_gain = target;
		} else if (g_gain < target) {
			g_gain += g_gain_step;
			if (g_gain > target) g_gain = target;
		}
		g_quiet_buf[i * 2]     = (int16_t)(in[i * 2] * g_gain);
		g_quiet_buf[i * 2 + 1] = (int16_t)(in[i * 2 + 1] * g_gain);
	}
	return g_quiet_buf;
}

static void push(const int16_t *f, size_t n)
{
	size_t i, took;

	/* Every caller hands over at most a chunk - the resampler flushes at
	 * OUT_CHUNK and priming at 512 - so one scratch buffer is enough. */
	if ((g_quiet || g_gain < 1.0f) && n <= OUT_CHUNK) f = quieten(f, n);

	for (i = 0; i < n * 2; i++) {
		int v = f[i] < 0 ? -f[i] : f[i];
		if (v > g_peak) g_peak = v;
		if (v) g_nonzero++;
		g_sq += (double)v * v;
	}
	g_total += (uint64_t)n * 2;

	if (g_tap_out) fwrite(f, sizeof(int16_t) * 2, n, g_tap_out);
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
	if (g_tap_in) fwrite(f, sizeof(int16_t) * 2, n, g_tap_in);
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
	/* A session starts where quiet already is, with no fade: the launcher
	 * sends it before RUN, so a game started during the music is silent from
	 * its first sample rather than faded out of a first loud one. */
	g_gain = g_quiet ? 0.0f : 1.0f;
	g_gain_step = dst_rate > 0 ? 1000.0f / ((float)QUIET_FADE_MS * (float)dst_rate)
	                           : 1.0f;
	g_quiet_frames = 0;

	if (src_rate <= 0.0 || dst_rate <= 0) {
		g_base_ratio = g_ratio = 1.0;
		build_kernel();
		memset(g_buf, 0, sizeof g_buf);
		g_have = TAPS;
		g_pos  = HALF;
		return;
	}
	g_base_ratio = src_rate / (double)dst_rate;
	g_ratio      = g_base_ratio;
	g_capacity   = capacity_frames;

	build_kernel();
	/* Start with a window's worth of silence so the first real sample is
	 * filtered against something defined rather than against whatever the last
	 * game left behind. */
	memset(g_buf, 0, sizeof g_buf);
	g_have = TAPS;
	g_pos  = HALF;
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
	 * a persistent error, so the buffer stabilizes wherever the error happens to
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

static double sinc(double x)
{
	if (fabs(x) < 1e-12) return 1.0;
	x *= M_PI;
	return sin(x) / x;
}

/* Built once per session from the NOMINAL ratio. Rate control moves the real
 * ratio by at most 0.5%, which shifts the cutoff by less than a semitone's
 * worth of bandwidth at 22 kHz - inaudible, and not worth rebuilding a table
 * every frame to chase. */
static void build_kernel(void)
{
	int p, t;

	/* Downsampling has to lowpass at the DESTINATION Nyquist or the content
	 * above it folds back as aliasing; upsampling only has to reject the
	 * images above the SOURCE Nyquist. One expression covers both. */
	double fc = 0.5 * ROLLOFF;
	if (g_base_ratio > 1.0) fc /= g_base_ratio;

	for (p = 0; p < PHASES; p++) {
		double frac = (double)p / PHASES;
		double sum = 0.0;

		for (t = 0; t < TAPS; t++) {
			double x = (double)(t - HALF + 1) - frac;
			double u = (x + HALF) / (double)TAPS;      /* 0..1 across the window */
			double w = 0.42 - 0.50 * cos(2.0 * M_PI * u)
			                + 0.08 * cos(4.0 * M_PI * u);
			double v = 2.0 * fc * sinc(2.0 * fc * x) * w;

			g_kern[p][t] = (float)v;
			sum += v;
		}
		/* Normalize every phase to unity DC gain independently. Skipping this
		 * leaves each phase with a slightly different gain, and since the phase
		 * cycles at the resampling rate the difference becomes amplitude
		 * modulation - a tone at the beat frequency, which is exactly the kind
		 * of artifact this filter exists to remove. */
		if (sum != 0.0)
			for (t = 0; t < TAPS; t++) g_kern[p][t] /= (float)sum;
	}
}

static int16_t clip(double v)
{
	/* A windowed sinc has negative lobes, so the tap sum exceeds unity on a
	 * transient even though its DC gain is exactly one. Linear interpolation
	 * could never overshoot its inputs; this can, and unclamped it would wrap. */
	if (v >  32767.0) return  32767;
	if (v < -32768.0) return -32768;
	return (int16_t)(v > 0 ? v + 0.5 : v - 0.5);
}

size_t diatom_audio_push(const int16_t *in, size_t frames)
{
	int16_t out[OUT_CHUNK * 2];
	size_t produced = 0, total = 0;
	size_t keep;

	if (!in || !frames) return 0;

	/* Larger blocks than the buffer holds are split rather than truncated.
	 * Nothing observed emits more than ~1100 frames at once - mGBA's 65536 Hz
	 * over one frame is the worst - but a core is free to. */
	while (frames > MAX_IN) {
		total += diatom_audio_push(in, MAX_IN);
		in    += MAX_IN * 2;
		frames -= MAX_IN;
	}

	memcpy(&g_buf[g_have][0], in, frames * 2 * sizeof(int16_t));
	g_have += frames;

	/* There is NO pass-through path any more, and its absence is the point.
	 *
	 * A FIR delays by half its length; a straight copy does not. Switching
	 * between them mid-stream jumps the output by 16 samples, which is a click.
	 * FCEUmm reports 48000 into a 48000 device, so its nominal ratio is exactly
	 * 1.0 and rate control walks the real one back and forth across that
	 * boundary continuously - it would have clicked constantly. The old code
	 * had a narrower version of this bug and patched it by carrying one sample
	 * across the seam; removing the seam removes the class.
	 *
	 * What it costs at ratio 1.0 is a lowpass at ~22 kHz and 64 multiplies per
	 * frame. Neither is worth a mode switch.
	 *
	 * Produce only where the whole window fits. `g_pos` persists across calls:
	 * an earlier version restarted it each block and dropped the pending output
	 * frame, about one per call - a 0.125% leak, small enough to look like
	 * clock drift and large enough to drain the buffer in twenty seconds. */
	while ((size_t)((int)g_pos + HALF) < g_have) {
		int center = (int)g_pos;
		const float *k = g_kern[(int)((g_pos - center) * PHASES)];
		double al = 0.0, ar = 0.0;
		int t;

		for (t = 0; t < TAPS; t++) {
			const int16_t *sp = &g_buf[center + t - HALF + 1][0];
			al += sp[0] * (double)k[t];
			ar += sp[1] * (double)k[t];
		}
		out[produced * 2]     = clip(al);
		out[produced * 2 + 1] = clip(ar);
		produced++;
		g_pos += g_ratio;

		if (produced == OUT_CHUNK) {
			push(out, produced);
			total += produced;
			produced = 0;
		}
	}

	/* Slide the window down, keeping the history the next call's leftmost tap
	 * will reach for. */
	keep = (size_t)((int)g_pos - HALF + 1);
	if ((int)g_pos - HALF + 1 > 0 && keep <= g_have) {
		memmove(&g_buf[0][0], &g_buf[keep][0],
		        (g_have - keep) * 2 * sizeof(int16_t));
		g_have -= keep;
		g_pos  -= (double)keep;
	}

	if (produced) { push(out, produced); total += produced; }
	return total;
}
