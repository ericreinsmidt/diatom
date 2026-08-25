/* A stand-in launcher, driving Diatom over the ADR-0009 socket.
 *
 * Exists to measure the number the whole resident architecture rests on: how
 * long from sending RUN to receiving RUNNING, when the process is already up
 * and the core is already resident. That is what a player experiences as
 * "launch time" once PlayOS drives Diatom rather than spawning it.
 *
 * Also exercises the protocol itself - READY on connect, the RUNNING/EXIT
 * display handover, STOP mid-game - so a protocol regression fails here rather
 * than on a device with a launcher attached.
 *
 * An instrument, not Diatom code. See tools/README.md.
 *
 * `secs` 0 means do not send STOP: let the session end on its own, which is how
 * the crash paths are watched - the point there is exactly that Diatom reports
 * something nobody asked it to.
 *
 *   protodrive <socket> <secs> [--exercise <optkey> <optval>] <core>|<rom>[|<firmware>] ...
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static uint64_t us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000ull + t.tv_nsec / 1000;
}

static int fd = -1;
static char rbuf[4096];
static size_t rused;

/* Blocking read of one newline-terminated line. */
static char *rline(void)
{
	static char out[4096];
	for (;;) {
		char *nl = memchr(rbuf, '\n', rused);
		ssize_t n;
		if (nl) {
			size_t len = (size_t)(nl - rbuf);
			memcpy(out, rbuf, len);
			out[len] = '\0';
			memmove(rbuf, nl + 1, rused - len - 1);
			rused -= len + 1;
			return out;
		}
		n = read(fd, rbuf + rused, sizeof rbuf - rused - 1);
		if (n <= 0) return NULL;
		rused += (size_t)n;
	}
}

static void wline(const char *fmt, ...)
{
	char b[2048];
	va_list ap;
	int n;
	va_start(ap, fmt); n = vsnprintf(b, sizeof b - 2, fmt, ap); va_end(ap);
	b[n++] = '\n';
	if (write(fd, b, (size_t)n) < 0) perror("write");
}

int main(int argc, char **argv)
{
	struct sockaddr_un a;
	int secs, i, attempt, nopt = 0, exercise = 0, argi = 3, menu = 0, menus = 0;
	const char *optkey = "", *optval = "";

	/* Line-buffered, because this tool watches things that die. Redirected to
	 * a file stdout is block-buffered, so a protodrive killed while waiting
	 * loses everything it had already printed - and on 2026-08-25 that read as
	 * "the session never reached RUNNING" when it had. An instrument whose
	 * output disappears exactly when the interesting thing happens is worse
	 * than no instrument. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* protodrive <sock> <secs> --menu <spec>  waits for the player to press
	 * MENU, which is the one path no automated client can trigger. */
	if (argc > 3 && !strcmp(argv[3], "--menu")) { menu = 1; argi = 4; }

	/* Optional: protodrive <sock> <secs> --exercise <optkey> <optval> <spec>...
	 * Scanned in place rather than by shifting argv, which loses argv[1]. */
	if (argc > 5 && !strcmp(argv[3], "--exercise")) {
		exercise = 1; optkey = argv[4]; optval = argv[5]; argi = 6;
	}
	if (argc < 4) {
		fprintf(stderr, "usage: protodrive <socket> <secs> <core>|<rom> ...\n");
		return 1;
	}
	secs = atoi(argv[2]);

	memset(&a, 0, sizeof a);
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof a.sun_path, "%s", argv[1]);

	for (attempt = 0; attempt < 60; attempt++) {
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) break;
		close(fd); fd = -1;
		usleep(200000);
	}
	if (fd < 0) { fprintf(stderr, "protodrive: cannot connect to %s\n", argv[1]); return 2; }
	printf("<- %s\n", rline());

	for (i = argi; i < argc; i++) {
		char spec[2048], *bar, *rom, *fw;
		uint64_t t0, t_running = 0, t_last;
		const char *base;

		snprintf(spec, sizeof spec, "%s", argv[i]);
		bar = strchr(spec, '|');
		if (!bar) { fprintf(stderr, "bad spec: %s\n", argv[i]); continue; }
		*bar = '\0';
		rom = bar + 1;
		/* An optional third field: core|rom|firmware, so the ADR-0017 check can
		 * be driven from here rather than only from the command line. */
		fw = strchr(rom, '|');
		if (fw) { *fw = '\0'; fw++; }
		base = strrchr(rom, '/');
		base = base ? base + 1 : rom;

		t0 = us();
		t_last = t0;
		if (fw && *fw)
			wline("RUN\tcore=%s\trom=%s\tfirmware=%s", spec, rom, fw);
		else
			wline("RUN\tcore=%s\trom=%s", spec, rom);

		for (;;) {
			char *l = rline();
			if (!l) { fprintf(stderr, "connection closed\n"); return 3; }
			if (!strncmp(l, "OPTIONS", 7) || !strncmp(l, "OPTION\t", 7) ||
			    !strncmp(l, "OPTSET", 6) || !strncmp(l, "SAVED", 5) ||
			    !strncmp(l, "LOADED", 6)) {
				if (nopt < 4 || strncmp(l, "OPTION\t", 7)) printf("    <- %.100s\n", l);
				if (!strncmp(l, "OPTION\t", 7)) nopt++;
			} else if (!strncmp(l, "PAUSED", 6)) {
				/* Elapsed time matters: two PAUSED events milliseconds apart
				 * are one press re-triggering, seconds apart are two presses.
				 * Without this the log cannot tell them apart, and a real bug
				 * on 2026-08-25 was read as correct behaviour because of it. */
				uint64_t now = us();
				menus++;
				printf("  <- PAUSED   (menu %d, +%.2fs since last event)\n",
				       menus, (now - t_last) / 1000000.0);
				t_last = now;
				if (menus == 1) {
					printf("     driving: SAVE, then RESUME\n");
					wline("SAVE\tpath=/mnt/SDCARD/diatom/menu.state");
					wline("RESUME");
				} else {
					printf("     driving: STOP\n");
					wline("STOP");
				}
			} else if (!strncmp(l, "RUNNING", 7)) {
				if (menu && t_running) {
					printf("  <- RUNNING  (+%.2fs) - Diatom has the display back\n",
				       (us() - t_last) / 1000000.0);
				t_last = us();
					continue;
				}
				t_running = us() - t0;
				printf("  RUN -> RUNNING %8.1f ms   %.40s\n",
				       t_running / 1000.0, base);
				if (menu) {
					printf("     >>> press MENU on the device (twice: save+resume, then quit)\n");
					continue;
				}

				/* Exercise the launcher-facing surface: enumerate options,
				 * change one, write a state, read it back. */
				if (exercise) {
					wline("OPTIONS");
					sleep(1);
					wline("SETOPT\tkey=%s\tvalue=%s", optkey, optval);
					wline("SAVE\tpath=/mnt/SDCARD/diatom/proto.state");
					sleep(1);
					wline("LOAD\tpath=/mnt/SDCARD/diatom/proto.state");
					sleep(1);
				}
				if (secs == 0) {
					printf("     waiting for it to end by itself\n");
					continue;
				}
				sleep(secs);
				wline("STOP");
			} else if (!strncmp(l, "EXIT", 4)) {
				/* Elapsed since RUNNING, because for a crash that is the
				 * whole measurement: it says the report arrived from a
				 * running game rather than from the launch failing. */
				printf("  <- %s   (+%.2fs)\n", l, (us() - t_last) / 1000000.0);
				break;
			} else if (!strncmp(l, "ERROR", 5)) {
				printf("  <- %s   (+%.2fs, launcher keeps the display)\n",
				       l, (us() - t_last) / 1000000.0);
				break;
			}
		}
	}
	wline("QUIT");
	printf("sent QUIT\n");
	close(fd);
	return 0;
}
