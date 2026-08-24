/* Where the picture goes.
 *
 * ADR-0007 splits this deliberately: deciding the factor is arithmetic, so it
 * lives here once and every port behaves identically. Performing the blit is
 * hardware, so it belongs to the port. Put both in the port and the maths gets
 * duplicated per device and drifts.
 *
 * Integer scaling only, per register §5. Both known panels take clean factors
 * for every system in scope - mostly. Measured counter-example: PC Engine's
 * 256x243 at 3x is 768x729, which exceeds the Miniloong's 720 lines but fits
 * the Brick's 768. Same system, different factor per device. That is the case
 * this function exists to handle rather than assume away.
 */
#include "diatom.h"

diatom_rect diatom_scale_rect(int src_w, int src_h, int surf_w, int surf_h)
{
	diatom_rect r;
	int fx, fy, f;

	if (src_w <= 0 || src_h <= 0 || surf_w <= 0 || surf_h <= 0) {
		r.x = r.y = r.w = r.h = 0;
		return r;
	}

	fx = surf_w / src_w;
	fy = surf_h / src_h;
	f  = fx < fy ? fx : fy;

	/* Source larger than the surface. Integer scaling cannot help, so show it
	 * 1:1 and let the port clip rather than refusing the frame outright.
	 * Reachable today: SNES hires is 512x448, and 2x would need 1024x896. */
	if (f < 1) f = 1;

	r.w = src_w * f;
	r.h = src_h * f;
	r.x = (surf_w - r.w) / 2;
	r.y = (surf_h - r.h) / 2;
	return r;
}
