/* SPIKE, part two: the realistic pause state.
 *
 * A paused Diatom does not exit - ADR-0008 makes it long-lived. So it still
 * holds /dev/fb0 open and mmapped while the launcher draws through EGL. Part
 * one only proved handoff works when the fbdev side fully releases.
 *
 * pan a while -> HOLD (open and mapped, not panning) -> pan again.
 * Run the EGL presenter during the hold.
 *
 * An instrument, not Diatom code. Used by the display-handoff spike. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static struct fb_var_screeninfo v;
static struct fb_fix_screeninfo f;

static int pan(int fd, int page)
{
	v.yoffset  = (uint32_t)page * v.yres;
	v.activate = FB_ACTIVATE_VBL;
	return ioctl(fd, FBIOPAN_DISPLAY, &v);
}

int main(int argc, char **argv)
{
	int hold = argc > 1 ? atoi(argv[1]) : 6;
	int fd, i, fails = 0;
	uint8_t *fb;
	size_t sz;

	fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) { perror("open fb0"); return 1; }
	ioctl(fd, FBIOGET_FSCREENINFO, &f);
	ioctl(fd, FBIOGET_VSCREENINFO, &v);
	sz = (size_t)3 * v.yres * f.line_length;
	fb = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) { perror("mmap"); return 2; }
	memset(fb, 0x30, sz);

	for (i = 0; i < 60; i++) if (pan(fd, i % 3) != 0) fails++;
	printf("HOLD: panned 60, %d fails. holding fb0 open+mapped for %ds\n",
	       fails, hold);
	fflush(stdout);

	sleep(hold);            /* EGL runs in here, while we still hold fb0 */

	fails = 0;
	for (i = 0; i < 60; i++) if (pan(fd, i % 3) != 0) fails++;
	printf("HOLD: resumed panning after EGL, %d fails\n", fails);

	munmap(fb, sz);
	close(fd);
	return 0;
}
