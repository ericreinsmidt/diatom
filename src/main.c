/* Diatom — a minimal libretro frontend.
 *
 * Standalone is the primary mode:  diatom --core X.so --rom game.nes
 * A host application driving a resident Diatom over a socket (ADR-0009) is an
 * additional mode, not the fundamental one. Designing for a program that stands
 * alone is a stricter test than designing for one embedder.
 *
 * The frame is copied inside the core's video callback and presented AFTER
 * retro_run returns — ADR-0007. Cores emit video and audio from inside
 * retro_run in an order that varies, so presenting from the callback means
 * committing to a frame before knowing how much audio it produced.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
static diatom_rect      g_dst;
static int              g_dst_src_w, g_dst_src_h;   /* what g_dst was computed for */

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
	const char *core_path = NULL, *rom_path = NULL;
	struct retro_system_av_info av;
	struct retro_system_info si;
	uint64_t frame_us, next_us, t_start;
	long limit = 0, frames = 0, geom_changes = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--core") && i + 1 < argc) core_path = argv[++i];
		else if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom_path = argv[++i];
		else if (!strcmp(argv[i], "--system") && i + 1 < argc) g_policy.system_dir = argv[++i];
		else if (!strcmp(argv[i], "--save") && i + 1 < argc) g_policy.save_dir = argv[++i];
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) limit = strtol(argv[++i], NULL, 10);
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

	/* Genesis 3-button vs 6-button is a correctness issue, not a preference —
	 * some early games misbehave with a 6-button pad attached, which is why the
	 * real pad has a Mode switch. Digital-only (ADR-0003) removes axes, not
	 * device types. */
	g_core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

	g_core.get_system_av_info(&av);
	printf("diatom: %ux%u (max %ux%u) %.4f fps, %.0f Hz -> %d Hz\n",
	       av.geometry.base_width, av.geometry.base_height,
	       av.geometry.max_width, av.geometry.max_height,
	       av.timing.fps, av.timing.sample_rate, g_caps.audio_rate);

	diatom_audio_configure(av.timing.sample_rate, g_caps.audio_rate);

	/* Provisional pacing: sleep to the core's own rate. Register §7 is the real
	 * problem — nothing runs at 60Hz and PAL at 50.0070 is a deliberate target,
	 * so this drifts against the panel. It is honest enough to watch a game and
	 * nothing more. */
	frame_us = (uint64_t)(1000000.0 / (av.timing.fps > 0 ? av.timing.fps : 60.0));
	next_us  = diatom_port_now_us();

	t_start = diatom_port_now_us();

	while (!diatom_port_should_quit() && (limit <= 0 || frames < limit)) {
		uint64_t now;

		g_frame_fresh = false;
		g_core.run();
		frames++;

		if (diatom_env_geometry_changed()) { g_dst_src_w = 0; geom_changes++; }

		if (g_frame_w != g_dst_src_w || g_frame_h != g_dst_src_h) {
			g_dst = diatom_scale_rect(g_frame_w, g_frame_h,
			                          g_caps.surface_w, g_caps.surface_h);
			g_dst_src_w = g_frame_w;
			g_dst_src_h = g_frame_h;
		}

		diatom_port_present(g_frame_fresh ? g_frame : NULL,
		                    g_frame_w, g_frame_h, g_frame_pitch,
		                    g_policy.pixfmt, g_dst);

		next_us += frame_us;
		now = diatom_port_now_us();
		if (next_us > now) {
			struct timespec ts;
			uint64_t d = next_us - now;
			ts.tv_sec  = (time_t)(d / 1000000);
			ts.tv_nsec = (long)((d % 1000000) * 1000);
			nanosleep(&ts, NULL);
		} else {
			next_us = now;      /* fell behind; do not accumulate debt */
		}
	}

	{
		double secs = (diatom_port_now_us() - t_start) / 1000000.0;
		printf("diatom: %ld frames in %.2fs = %.2f fps (target %.4f)\n",
		       frames, secs, secs > 0 ? frames / secs : 0.0, av.timing.fps);
		printf("diatom: %ld geometry change(s), last rect %dx%d at %d,%d\n",
		       geom_changes, g_dst.w, g_dst.h, g_dst.x, g_dst.y);
		printf("diatom: audio queued at exit %zu frames (capacity %d)\n",
		       diatom_port_audio_queued(), g_caps.audio_buffer_frames);
	}

	diatom_core_stop(&g_core);
	/* No dlclose. Ever. ADR-0006. */
	diatom_port_shutdown();
	free(g_frame);
	return 0;
}
