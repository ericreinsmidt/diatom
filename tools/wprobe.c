/* SPIKE: what does an atomic save-file write actually cost on this SD card?
 * write to .tmp, fsync, rename - the sequence a crash-safe save must use.
 * An instrument, not Diatom code. Its numbers are cited by ADR-0016. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t us(void)
{
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000ull + t.tv_nsec / 1000;
}

static uint64_t once(const char *dir, void *buf, size_t n, int do_sync)
{
	char tmp[512], fin[512];
	uint64_t t0;
	int fd;

	snprintf(tmp, sizeof tmp, "%s/wprobe.tmp", dir);
	snprintf(fin, sizeof fin, "%s/wprobe.dat", dir);

	t0 = us();
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) return 0;
	if (write(fd, buf, n) != (ssize_t)n) { close(fd); return 0; }
	if (do_sync) fsync(fd);
	close(fd);
	rename(tmp, fin);
	return us() - t0;
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : ".";
	size_t sizes[] = { 2048, 8192, 131072, 528448 };
	void *buf = malloc(1 << 20);
	int s, i, N = 20;

	memset(buf, 0xa5, 1 << 20);
	printf("%-10s %-8s %8s %8s %8s\n", "size", "fsync", "min", "avg", "max");
	for (s = 0; s < 4; s++) {
		int sync;
		for (sync = 1; sync >= 0; sync--) {
			uint64_t mn = ~0ull, mx = 0, sum = 0, d;
			for (i = 0; i < N; i++) {
				d = once(dir, buf, sizes[s], sync);
				if (d < mn) mn = d;
				if (d > mx) mx = d;
				sum += d;
			}
			printf("%-10zu %-8s %6llu u %6llu u %6llu u\n", sizes[s],
			       sync ? "yes" : "no",
			       (unsigned long long)mn,
			       (unsigned long long)(sum / N),
			       (unsigned long long)mx);
		}
	}
	free(buf);
	return 0;
}
