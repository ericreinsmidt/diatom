/* Can a port read and set the backlight with no vendor headers?
 *
 * PlayOS's libmsettings shows brightness going through /dev/disp rather than a
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

int main(int argc, char **argv)
{
	unsigned long a[4] = { 0, 0, 0, 0 };
	int fd = open("/dev/disp", O_RDWR);
	int v;

	if (fd < 0) { perror("open /dev/disp"); return 2; }

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
