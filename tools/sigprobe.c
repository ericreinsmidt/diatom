/* SPIKE: when this device powers down, does a running process get a signal,
 * and how long does it get before SIGKILL?
 *
 * Everything is written with write()+fsync() straight to the card, including
 * from inside the handler, because the whole point is surviving a power cut.
 * Heartbeats every 200ms give a time of death even if no signal ever arrives.
 *
 * An instrument, not Diatom code. Its numbers are cited by ADR-0016. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int      g_fd = -1;
static uint64_t g_t0;

static uint64_t us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000ull + t.tv_nsec / 1000;
}

/* write()+fsync() are async-signal-safe; snprintf is not strictly, but the
 * alternative is hand-rolling itoa and the practical risk here is nil. */
static void say(const char *what, int n)
{
	char line[96];
	int  len = snprintf(line, sizeof line, "T+%llu.%03llus  %s %d\n",
	                    (unsigned long long)((us() - g_t0) / 1000000),
	                    (unsigned long long)(((us() - g_t0) / 1000) % 1000),
	                    what, n);
	if (g_fd >= 0 && len > 0) { write(g_fd, line, (size_t)len); fsync(g_fd); }
}

static void handler(int sig) { say("SIGNAL", sig); }

int main(int argc, char **argv)
{
	static const int sigs[] = { SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGUSR1,
	                            SIGUSR2, SIGPWR, SIGALRM, SIGTSTP };
	struct sigaction sa;
	size_t i;
	long beat = 0;

	g_fd = open(argc > 1 ? argv[1] : "/mnt/SDCARD/diatom/sigtest.log",
	            O_WRONLY | O_CREAT | O_APPEND, 0644);
	g_t0 = us();

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = handler;
	sa.sa_flags   = SA_RESTART;
	for (i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
		sigaction(sigs[i], &sa, NULL);

	say("START pid", (int)getpid());

	for (;;) {
		struct timespec nap = { 0, 200 * 1000 * 1000 };
		nanosleep(&nap, NULL);
		say("alive beat", (int)++beat);
	}
}
