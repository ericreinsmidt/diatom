/* What does FBIOPAN_DISPLAY on the Brick's disp2 fbdev actually cost, and
 * does anything change it?
 *
 * An instrument, not Diatom code. Its numbers are cited by ADR-0013. */
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, uint32_t)
#endif

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}

static void run(const char *name, int fd, struct fb_var_screeninfo *v,
                uint32_t activate, int sleep_us, int alternate, int n)
{
	uint64_t min = ~0ull, max = 0, sum = 0;
	int i, fails = 0;

	for (i = 0; i < n; i++) {
		uint64_t t0, dt;
		if (sleep_us) usleep(sleep_us);
		v->yoffset  = alternate ? (i & 1 ? v->yres : 0) : 0;
		v->activate = activate;
		t0 = now_us();
		if (ioctl(fd, FBIOPAN_DISPLAY, v) != 0) fails++;
		dt = now_us() - t0;
		if (dt < min) min = dt;
		if (dt > max) max = dt;
		sum += dt;
	}
	printf("%-28s min %6llu  avg %6llu  max %6llu us  (%d fails)\n",
	       name, (unsigned long long)min, (unsigned long long)(sum / n),
	       (unsigned long long)max, fails);
}

int main(void)
{
	struct fb_var_screeninfo v;
	int fd = open("/dev/fb0", O_RDWR);
	int i;

	if (fd < 0) { perror("fb0"); return 1; }
	if (ioctl(fd, FBIOGET_VSCREENINFO, &v) != 0) { perror("vscreeninfo"); return 1; }
	printf("fb %ux%u virtual %ux%u activate 0x%x\n",
	       v.xres, v.yres, v.xres_virtual, v.yres_virtual, v.activate);

	run("pan tight alternate", fd, &v, v.activate, 0, 1, 60);
	run("pan tight same-offset", fd, &v, v.activate, 0, 0, 60);
	run("pan +8ms sleep", fd, &v, v.activate, 8000, 1, 30);
	run("pan +16ms sleep", fd, &v, v.activate, 16000, 1, 30);
	run("pan ACTIVATE_NOW", fd, &v, FB_ACTIVATE_NOW, 0, 1, 30);
	run("pan ACTIVATE_VBL", fd, &v, FB_ACTIVATE_VBL, 0, 1, 30);
	run("pan NOW|NOWAIT", fd, &v, FB_ACTIVATE_NOW | 0x4000 /* FB_ACTIVATE_NOWAIT? no such flag; harmless */, 0, 1, 30);

	{
		uint64_t min = ~0ull, max = 0, sum = 0;
		int fails = 0;
		for (i = 0; i < 30; i++) {
			uint32_t zero = 0;
			uint64_t t0 = now_us(), dt;
			if (ioctl(fd, FBIO_WAITFORVSYNC, &zero) != 0) fails++;
			dt = now_us() - t0;
			if (dt < min) min = dt;
			if (dt > max) max = dt;
			sum += dt;
		}
		printf("%-28s min %6llu  avg %6llu  max %6llu us  (%d fails)\n",
		       "WAITFORVSYNC", (unsigned long long)min,
		       (unsigned long long)(sum / 30), (unsigned long long)max, fails);
	}

	v.yoffset = 0; v.activate = FB_ACTIVATE_NOW;
	ioctl(fd, FBIOPAN_DISPLAY, &v);
	close(fd);
	return 0;
}
