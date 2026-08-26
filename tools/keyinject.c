/* Inject one button press into an evdev node, so the pause/menu path can be
 * exercised over adb with nobody holding the device. The kernel forwards
 * events written to an evdev fd through the input core, so every reader -
 * SDL in Diatom, the raw fds in a launcher - sees them as if the pad sent
 * them.
 *
 *   keyinject /dev/input/event3 press 316     press+release BTN_MODE (MENU)
 *   keyinject /dev/input/event3 hat -1        d-pad up (HAT0Y pulse)
 *
 * An instrument. See tools/README.md.
 */
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void put(int fd, unsigned short type, unsigned short code, int value)
{
	struct input_event ev;
	memset(&ev, 0, sizeof ev);
	ev.type = type; ev.code = code; ev.value = value;
	if (write(fd, &ev, sizeof ev) != (ssize_t)sizeof ev) perror("write");
	memset(&ev, 0, sizeof ev);
	ev.type = EV_SYN; ev.code = SYN_REPORT;
	if (write(fd, &ev, sizeof ev) != (ssize_t)sizeof ev) perror("write syn");
}

int main(int argc, char **argv)
{
	int fd;

	if (argc < 4) {
		fprintf(stderr, "usage: keyinject <dev> press <code> | hat <val>\n");
		return 1;
	}
	fd = open(argv[1], O_WRONLY);
	if (fd < 0) { perror(argv[1]); return 2; }

	if (!strcmp(argv[2], "press")) {
		put(fd, EV_KEY, (unsigned short)atoi(argv[3]), 1);
		usleep(90 * 1000);
		put(fd, EV_KEY, (unsigned short)atoi(argv[3]), 0);
	} else if (!strcmp(argv[2], "hat")) {
		put(fd, EV_ABS, ABS_HAT0Y, atoi(argv[3]));
		usleep(90 * 1000);
		put(fd, EV_ABS, ABS_HAT0Y, 0);
	} else if (!strcmp(argv[2], "hatx")) {
		put(fd, EV_ABS, ABS_HAT0X, atoi(argv[3]));
		usleep(90 * 1000);
		put(fd, EV_ABS, ABS_HAT0X, 0);
	}
	close(fd);
	return 0;
}
