/* Diatom - a minimal libretro frontend.
 *
 * Standalone is the primary mode:  diatom --core X.so --rom game.nes
 * A host application driving a resident Diatom over a socket (ADR-0009) is an
 * additional mode, not the fundamental one. Designing for a program that stands
 * alone is a stricter test than designing for one embedder.
 *
 * The frame is copied inside the core's video callback and presented AFTER
 * retro_run returns - ADR-0007. Cores emit video and audio from inside
 * retro_run in an order that varies, so presenting from the callback means
 * committing to a frame before knowing how much audio it produced.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "diatom.h"

/* --- the frame the core last handed us ----------------------------------- */
static void       *g_frame;
static size_t      g_frame_cap;
static int         g_frame_w, g_frame_h;
static size_t      g_frame_pitch;
static bool        g_frame_fresh;

static diatom_core      g_core;
static diatom_policy    g_policy;
static diatom_port_caps g_caps;
static diatom_rect      g_dst;   /* locked at load - ADR-0011 */

/* Called from inside retro_run. Copy and return; do not present here. */
void diatom_on_video(const void *data, unsigned w, unsigned h, size_t pitch)
{
	size_t need;

	if (!data) return;          /* dupe: keep the previous frame */
	need = pitch * h;
	if (need > g_frame_cap) {
		void *p = realloc(g_frame, need);
		if (!p) return;
		g_frame = p;
		g_frame_cap = need;
	}
	memcpy(g_frame, data, need);
	g_frame_w = (int)w;
	g_frame_h = (int)h;
	g_frame_pitch = pitch;
	g_frame_fresh = true;
}

void diatom_on_audio_batch_store(const int16_t *data, size_t frames)
{
	diatom_audio_push(data, frames);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: diatom --core <core.so> --rom <file> [--system <dir>] [--save <dir>]\n");
}

int main(int argc, char **argv)
{
	const char *core_path = NULL, *rom_path = NULL, *shot_path = NULL;
	struct retro_system_av_info av;
	struct retro_system_info si;
	double   frame_us, next_us;
	uint64_t t_start;
	long limit = 0, frames = 0, geom_changes = 0, resyncs = 0;
	size_t q_min = (size_t)-1, q_max = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--core") && i + 1 < argc) core_path = argv[++i];
		else if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom_path = argv[++i];
		else if (!strcmp(argv[i], "--system") && i + 1 < argc) g_policy.system_dir = argv[++i];
		else if (!strcmp(argv[i], "--save") && i + 1 < argc) g_policy.save_dir = argv[++i];
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) limit = strtol(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot_path = argv[++i];
		else { usage(); return 1; }
	}
	if (!core_path) { usage(); return 1; }

	if (!g_policy.system_dir) g_policy.system_dir = ".";
	if (!g_policy.save_dir)   g_policy.save_dir   = ".";
	g_policy.pixfmt = DIATOM_PIX_RGB565;   /* libretro's default is 0RGB1555,
	                                          which Diatom refuses; every core
	                                          measured picked RGB565 anyway */

	if (!diatom_port_init(&g_caps)) {
		fprintf(stderr, "diatom: port init failed\n");
		return 2;
	}

	if (!diatom_core_open(&g_core, core_path)) return 3;
	diatom_env_bind(&g_core, &g_policy, &g_caps);

	g_core.get_system_info(&si);
	printf("diatom: %s %s\n",
	       si.library_name ? si.library_name : "?",
	       si.library_version ? si.library_version : "?");

	if (!rom_path && !g_policy.supports_no_game) {
		fprintf(stderr, "diatom: this core needs content; pass --rom\n");
		return 4;
	}
	if (!diatom_core_start(&g_core, rom_path)) return 4;

	/* Genesis 3-button vs 6-button is a correctness issue, not a preference -
	 * some early games misbehave with a 6-button pad attached, which is why the
	 * real pad has a Mode switch. Digital-only (ADR-0003) removes axes, not
	 * device types. */
	g_core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

	g_core.get_system_av_info(&av);
	printf("diatom: %ux%u (max %ux%u) %.4f fps, %.0f Hz -> %d Hz\n",
	       av.geometry.base_width, av.geometry.base_height,
	       av.geometry.max_width, av.geometry.max_height,
	       av.timing.fps, av.timing.sample_rate, g_caps.audio_rate);

	diatom_audio_configure(av.timing.sample_rate, g_caps.audio_rate,
	                       g_caps.audio_buffer_frames);
	diatom_audio_prime();

	/* The display rect is locked here, from BASE geometry, and does not move
	 * again - ADR-0011.
	 *
	 * Cores announce hires by calling SET_GEOMETRY with a larger base_width
	 * mid-run (measured: 3 of 6 do this). Recomputing an integer factor from
	 * the new width collapses 3x to 1x and the picture shrinks to a fifth.
	 * Holding the rect fixed keeps the picture the same size AND is more
	 * faithful: SNES and PC Engine hires pixels are physically half-width, so
	 * 512 columns belong in the same screen width as 256. */
	g_dst = diatom_scale_rect(av.geometry.base_width, av.geometry.base_height,
	                          g_caps.surface_w, g_caps.surface_h);
	printf("diatom: display rect locked %dx%d at %d,%d\n",
	       g_dst.w, g_dst.h, g_dst.x, g_dst.y);

	/* Pace against a monotonic clock at the core's own rate, on an ABSOLUTE
	 * schedule kept in floating point.
	 *
	 * Absolute matters: an incremental "sleep frame_us each time" accumulates
	 * every scheduler overshoot forever, while a running deadline absorbs them -
	 * a long sleep is followed by a correspondingly short one.
	 *
	 * Floating point matters too, if less: 1000000/59.7275 is 16742.63us, and
	 * truncating loses 38ms per hour.
	 *
	 * Audio drift is not this loop's problem. It is handled by rate control,
	 * because no panel and no core will ever agree on a rate. */
	frame_us = 1000000.0 / (av.timing.fps > 0 ? av.timing.fps : 60.0);

	/* Warm up before starting the clock. The first frames create the texture,
	 * fault in code paths and prime the audio device; measured, they overrun the
	 * frame budget badly enough to trip a resync every single run. Timing them
	 * reports a rate the loop never actually sustains - and, worse, hides
	 * whether the steady-state loop is correct. */
	{
		uint64_t w0 = diatom_port_now_us();
		int w;
		for (w = 0; w < 3; w++) {
			g_core.run();
			diatom_port_present(g_frame, g_frame_w, g_frame_h, g_frame_pitch,
			                    g_policy.pixfmt, g_dst);
		}
		printf("diatom: warmup 3 frames in %.1f ms\n",
		       (diatom_port_now_us() - w0) / 1000.0);
	}

	t_start = diatom_port_now_us();
	next_us = (double)t_start;

	while (!diatom_port_should_quit() && (limit <= 0 || frames < limit)) {
		uint64_t now;

		g_frame_fresh = false;
		g_core.run();
		frames++;

		/* Noted, not acted on: the rect is locked (ADR-0011). The port scales
		 * whatever arrives into it, so a hires frame keeps its screen size. */
		if (diatom_env_geometry_changed()) geom_changes++;

		diatom_port_present(g_frame_fresh ? g_frame : NULL,
		                    g_frame_w, g_frame_h, g_frame_pitch,
		                    g_policy.pixfmt, g_dst);

		{
			size_t q = diatom_port_audio_queued();
			if (q < q_min) q_min = q;
			if (q > q_max) q_max = q;
		}
		diatom_audio_sync();

		next_us += frame_us;
		now = diatom_port_now_us();

		if (next_us > (double)now) {
			struct timespec ts;
			double d = next_us - (double)now;
			ts.tv_sec  = (time_t)(d / 1000000.0);
			ts.tv_nsec = (long)(fmod(d, 1000000.0) * 1000.0);
			nanosleep(&ts, NULL);
		} else if ((double)now - next_us > frame_us * 4.0) {
			/* More than four frames behind. Something stalled - the scheduler,
			 * a page fault, a slow core - and trying to catch up would just run
			 * fast for a while, which looks worse than dropping the debt.
			 * Counted, because a loop that resyncs often is a loop that is
			 * lying about its frame rate. */
			next_us = (double)now;
			resyncs++;
		}
		/* Otherwise keep the debt and let the next short sleep repay it. */
	}

	if (shot_path)
		printf("diatom: capture %s: %s\n", shot_path,
		       diatom_port_capture(shot_path) ? "ok" : "FAILED");

	{
		double secs = (diatom_port_now_us() - t_start) / 1000000.0;
		printf("diatom: %ld frames in %.2fs = %.2f fps (target %.4f)\n",
		       frames, secs, secs > 0 ? frames / secs : 0.0, av.timing.fps);
		printf("diatom: %ld geometry change(s), last rect %dx%d at %d,%d\n",
		       geom_changes, g_dst.w, g_dst.h, g_dst.x, g_dst.y);
		printf("diatom: %ld resync(s)\n", resyncs);
		printf("diatom: audio queued min %zu max %zu final %zu, target %d, capacity %d\n",
		       q_min == (size_t)-1 ? 0 : q_min, q_max,
		       diatom_port_audio_queued(),
		       g_caps.audio_buffer_frames / 2, g_caps.audio_buffer_frames);
		printf("diatom: rate control drift %+.3f%% (max %+.3f%%)\n",
		       diatom_audio_ratio_drift() * 100.0, 0.5);
	}

	diatom_core_stop(&g_core);
	/* No dlclose. Ever. ADR-0006. */
	diatom_port_shutdown();
	free(g_frame);
	return 0;
}
