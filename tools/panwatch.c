/* Who is panning /dev/fb0, and when?
 *
 * A tear at a display handover is two processes panning one framebuffer
 * inside the same refresh. That is invisible to any sampling slow enough to
 * see only steady states - `cat` in a shell loop reports "game" then "shelf"
 * and looks perfectly clean either way.
 *
 * This reads the pan offset in a tight loop and records only CHANGES, with
 * microsecond timestamps. One handover should be one transition. Several
 * transitions inside a few tens of milliseconds is contention: the two sides
 * taking the display from each other repeatedly.
 *
 *   panwatch <seconds>
 *
 * An instrument. See tools/README.md.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	struct timespec t0, now;
	char buf[64], last[64] = "";
	double secs = argc > 1 ? atof(argv[1]) : 10.0;
	long n = 0;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		int fd = open("/sys/class/graphics/fb0/pan", O_RDONLY);
		ssize_t r;
		double el;

		if (fd < 0) { perror("pan"); return 1; }
		r = read(fd, buf, sizeof buf - 1);
		close(fd);
		if (r <= 0) continue;
		buf[r] = '\0';
		while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == ' ')) buf[--r] = '\0';

		clock_gettime(CLOCK_MONOTONIC, &now);
		el = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9;
		if (strcmp(buf, last)) {
			printf("%8.3f  %s\n", el, buf);
			fflush(stdout);
			snprintf(last, sizeof last, "%s", buf);
			n++;
		}
		if (el > secs) break;
	}
	fprintf(stderr, "panwatch: %ld transition(s) in %.1fs\n", n, secs);
	return 0;
}
