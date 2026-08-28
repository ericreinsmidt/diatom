/* Can a port read and set the backlight with no vendor headers?
 *
 * TortOS's libmsettings shows brightness going through /dev/disp rather than a
 * sysfs backlight class - this device has no /sys/class/backlight at all. The
 * Allwinner disp2 driver takes plain command numbers with an unsigned long[4]
 * argument block, not _IOWR-encoded requests, so there is no struct size to get
 * wrong; the risk is the command numbers themselves.
 *
 * Same reason mixprobe exists: find out against the running kernel before any
 * of it reaches port/brick.c.
 *
 *   dispprobe          read brightness
 *   dispprobe <n>      set it, read back, and report
 *   dispprobe --sweep  write a ladder of low values, read each back, restore
 *
 * The sweep exists for a narrower question than the other two: port/brick.c
 * floors brightness at 8/255, and 8 was CHOSEN, not measured. If the driver
 * clamps low values itself then the floor is a hardware fact and belongs in
 * the port; if it stores whatever it is given, the floor is a judgment about
 * what a person can still see and needs eyes rather than an ioctl. Find out
 * which before spending anyone's attention on it.
 *
 * An instrument. See tools/README.md.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DISP_LCD_SET_BRIGHTNESS 0x102
#define DISP_LCD_GET_BRIGHTNESS 0x103

static int get(int fd)
{
	unsigned long a[4] = { 0, 0, 0, 0 };
	return ioctl(fd, DISP_LCD_GET_BRIGHTNESS, a);
}

static int set(int fd, unsigned long n)
{
	unsigned long a[4] = { 0, n, 0, 0 };
	return ioctl(fd, DISP_LCD_SET_BRIGHTNESS, a);
}

/* Every value the port's 20-step scale can land on at the bottom, plus the
 * ones below its floor that it currently refuses to use. */
static const unsigned long ladder[] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 13, 16, 20, 26, 32, 38, 64, 128, 255
};

static int sweep(int fd)
{
	unsigned long i;
	int was = get(fd), back;

	if (was < 0) { perror("GET_BRIGHTNESS"); return 1; }
	printf("was = %d\n", was);

	for (i = 0; i < sizeof ladder / sizeof ladder[0]; i++) {
		if (set(fd, ladder[i]) < 0) {
			printf("%3lu -> SET failed\n", ladder[i]);
			continue;
		}
		back = get(fd);
		printf("%3lu -> %d%s\n", ladder[i], back,
		       back == (int)ladder[i] ? "" : "   CLAMPED");
	}

	/* Restore before reporting, and verify it took: if writing 0 disabled the
	 * backlight rather than setting a duty cycle, this is where that shows. */
	set(fd, (unsigned long)was);
	back = get(fd);
	printf("restored = %d%s\n", back, back == was ? "" : "   RESTORE FAILED");

	/* Clamping is the finding, not a failure. Only a botched restore is. */
	return back == was ? 0 : 1;
}

int main(int argc, char **argv)
{
	unsigned long a[4] = { 0, 0, 0, 0 };
	int fd = open("/dev/disp", O_RDWR);
	int v;

	if (fd < 0) { perror("open /dev/disp"); return 2; }

	if (argc > 1 && argv[1][0] == '-') return sweep(fd);

	v = ioctl(fd, DISP_LCD_GET_BRIGHTNESS, a);
	if (v < 0) { perror("GET_BRIGHTNESS"); return 1; }
	printf("brightness = %d\n", v);

	if (argc > 1) {
		a[1] = strtoul(argv[1], NULL, 10);
		if (ioctl(fd, DISP_LCD_SET_BRIGHTNESS, a) < 0) { perror("SET"); return 1; }
		a[1] = 0;
		v = ioctl(fd, DISP_LCD_GET_BRIGHTNESS, a);
		printf("after write = %d\n", v);
	}
	close(fd);
	return 0;
}
