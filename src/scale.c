/* Where the picture goes.
 *
 * ADR-0007 splits this deliberately: deciding the rect is arithmetic, so it
 * lives here once and every port behaves identically. Performing the blit is
 * hardware, so it belongs to the port. Put both in the port and the maths gets
 * duplicated per device and drifts.
 *
 * Seven modes, because "how big should the picture be" has that many
 * defensible answers and no universally right one. The default is `stretch`
 * (ADR-0014), chosen by cycling them on a real panel rather than by argument.
 *
 * The counter-example that motivated per-device arithmetic still holds: PC
 * Engine's 256x243 at 3x is 768x729, which exceeds the Miniloong's 720 lines
 * but fits the Brick's 768. Same system, different factor per device.
 */
#include "diatom.h"

/* The comparison set, in cycle order on device. Ordered by how much of the
 * panel gets used, so stepping through is a single monotonic story rather than
 * a shuffle: boxed, boxed-but-shape-correct, shape-correct, that cropped to
 * cover, stretched, uniform-cropped, then 1:1 as a reference.
 *
 * Geometry and filter are cycled separately (mode on the shoulders, filter on
 * A) because the question worth answering is what a given geometry looks like
 * WITH and WITHOUT blending, and a single flat list makes that comparison two
 * presses apart instead of one. */
const diatom_display_mode_info diatom_modes[] = {
	{ "integer",   DIATOM_SCALE_INTEGER,
	  "largest whole factor, letterboxed" },
	{ "integer-vertical", DIATOM_SCALE_INTEGER_VERT,
	  "whole factor down, shape correct across" },
	{ "aspect",    DIATOM_SCALE_ASPECT_FIT,
	  "shape the core asks for, fits inside the panel" },
	{ "fill",      DIATOM_SCALE_ASPECT_FILL,
	  "shape kept, covers the panel, edges cropped" },
	{ "stretch",   DIATOM_SCALE_STRETCH,
	  "both axes filled, shape ignored" },
	{ "overscale", DIATOM_SCALE_INTEGER_OVER,
	  "next whole factor up: uniform pixels, edges cropped" },
	{ "native",    DIATOM_SCALE_NATIVE,
	  "1:1, no scaling at all" },
};
const int diatom_mode_count =
	(int)(sizeof diatom_modes / sizeof diatom_modes[0]);

/* Intended display aspect. Cores report one; libretro's own rule is that a
 * value <= 0 means "assume square pixels", so base geometry decides. NES
 * content is 256x240 square-pixel but reports 4:3, because a real NES pixel
 * was wider than tall on a CRT. */
static double target_aspect(int src_w, int src_h, double aspect)
{
	if (aspect > 0.0) return aspect;
	return (double)src_w / (double)src_h;
}

static diatom_rect centered(int w, int h, int surf_w, int surf_h)
{
	diatom_rect r;
	r.w = w;
	r.h = h;
	/* Truncating division can leave an odd remainder pixel at the bottom or
	 * right rather than splitting it. Nothing sane to do about a half pixel. */
	r.x = (surf_w - w) / 2;
	r.y = (surf_h - h) / 2;
	return r;
}

diatom_rect diatom_scale_rect(diatom_scale_mode mode, int src_w, int src_h,
                              double aspect, int surf_w, int surf_h)
{
	double a;
	int f, fx, fy;

	if (src_w <= 0 || src_h <= 0 || surf_w <= 0 || surf_h <= 0) {
		diatom_rect z = { 0, 0, 0, 0 };
		return z;
	}

	switch (mode) {
	case DIATOM_SCALE_NATIVE:
		return centered(src_w, src_h, surf_w, surf_h);

	case DIATOM_SCALE_INTEGER:
		fx = surf_w / src_w;
		fy = surf_h / src_h;
		f  = fx < fy ? fx : fy;
		/* Source larger than the surface. Integer scaling cannot help, so show
		 * it 1:1 and let the port clip rather than refusing the frame outright.
		 * Reachable today: SNES hires is 512x448, and 2x would need 1024x896. */
		if (f < 1) f = 1;
		return centered(src_w * f, src_h * f, surf_w, surf_h);

	case DIATOM_SCALE_INTEGER_VERT:
		/* Whole factor vertically, shape-correct horizontally.
		 *
		 * Exists because "integer scaling" does not mean "undistorted" - it
		 * means "source pixels preserved", and half the test matrix never had
		 * square pixels. Measured 2026-08-24 on a 1024x768 panel: plain
		 * integer shows NES 12.5% too narrow, SNES 14.3%, PC Engine 12.2%,
		 * because each reports a pixel aspect its resolution does not imply.
		 *
		 * This keeps the axis where uniformity is most visible exactly whole
		 * and lets the other axis carry the correction. For content that IS
		 * square-pixel - Game Boy, GBA, Genesis - it collapses to exactly what
		 * plain integer produces, so it is never the worse of the two.
		 *
		 * Step the factor down rather than clamp the width: a wide aspect on a
		 * narrow panel must lose a whole factor, not gain a squashed one. */
		a = target_aspect(src_w, src_h, aspect);
		for (f = surf_h / src_h; f > 1; f--) {
			int w = (int)((double)(src_h * f) * a + 0.5);
			if (w <= surf_w) break;
		}
		if (f < 1) f = 1;
		return centered((int)((double)(src_h * f) * a + 0.5), src_h * f,
		               surf_w, surf_h);

	case DIATOM_SCALE_INTEGER_OVER:
		/* Smallest integer factor that covers the surface on both axes, so the
		 * overflow is cropped. Keeps pixels perfectly uniform - the one way to
		 * fill a panel without uneven pixel rows - at the cost of the edges.
		 * On the Brick that is NES at 4x: 1024x960, losing 96 lines. Often the
		 * right trade, because the lines lost are the overscan a CRT hid. */
		fx = (surf_w + src_w - 1) / src_w;
		fy = (surf_h + src_h - 1) / src_h;
		f  = fx > fy ? fx : fy;
		if (f < 1) f = 1;
		return centered(src_w * f, src_h * f, surf_w, surf_h);

	case DIATOM_SCALE_ASPECT_FIT:
		a = target_aspect(src_w, src_h, aspect);
		if ((double)surf_w / a <= (double)surf_h)
			return centered(surf_w, (int)((double)surf_w / a + 0.5),
			               surf_w, surf_h);
		return centered((int)((double)surf_h * a + 0.5), surf_h,
		               surf_w, surf_h);

	case DIATOM_SCALE_ASPECT_FILL:
		a = target_aspect(src_w, src_h, aspect);
		if ((double)surf_w / a >= (double)surf_h)
			return centered(surf_w, (int)((double)surf_w / a + 0.5),
			               surf_w, surf_h);
		return centered((int)((double)surf_h * a + 0.5), surf_h,
		               surf_w, surf_h);

	case DIATOM_SCALE_STRETCH:
		return centered(surf_w, surf_h, surf_w, surf_h);
	}

	return centered(src_w, src_h, surf_w, surf_h);
}
