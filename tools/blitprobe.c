/* Is the blit slow because of the pixels it computes, or the memory it writes?
 *
 * present() costs 8.4 ms, against 1.65-4.2 ms for emulating the machine. That
 * makes it the largest single cost in the frame, and the disp2 hardware scaler
 * the obvious fix - but only if the cost is the WRITE. Framebuffer memory is
 * typically uncached or write-combining, and if that is the bottleneck then no
 * amount of NEON helps and hardware scaling is the only real answer. If instead
 * the cost is arithmetic, a better loop wins with none of the risk.
 *
 * Same loop, same pixel count, two destinations: the real framebuffer, and
 * ordinary malloc'd memory.
 *
 * An instrument, not Diatom code. See tools/README.md.
 */
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

#define SRC_W 256
#define SRC_H 224

static uint64_t us(void)
{
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000ull + t.tv_nsec / 1000;
}

static uint16_t src[SRC_W * SRC_H];
static int colmap[4096], rowmap[4096];

/* The same shape as port/brick.c: nearest-neighbor, RGB565 in, ARGB out. */
static void blit(uint32_t *dst, int dw, int dh, size_t stride_px)
{
	int x, y;
	for (y = 0; y < dh; y++) {
		const uint16_t *in = src + (size_t)rowmap[y] * SRC_W;
		uint32_t *out = dst + (size_t)y * stride_px;
		for (x = 0; x < dw; x++) {
			uint16_t c = in[colmap[x]];
			uint32_t r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
			out[x] = 0xff000000u
			       | (((r << 3) | (r >> 2)) << 16)
			       | (((g << 2) | (g >> 4)) << 8)
			       |  ((b << 3) | (b >> 2));
		}
	}
}

/* The same output, but converting each SOURCE pixel once instead of once per
 * destination pixel that samples it. At 4x horizontal and 3.4x vertical that is
 * roughly 13 redundant conversions per source pixel in the naive version.
 *
 * Two reuses: a scratch row holds the converted source row, and consecutive
 * destination rows that map to the same source row reuse it untouched. */
static uint32_t rowcache[4096];

static void blit_cached(uint32_t *dst, int dw, int dh, size_t stride_px)
{
	int x, y, cached = -1;
	for (y = 0; y < dh; y++) {
		uint32_t *out = dst + (size_t)y * stride_px;
		if (rowmap[y] != cached) {
			const uint16_t *in = src + (size_t)rowmap[y] * SRC_W;
			for (x = 0; x < SRC_W; x++) {
				uint16_t c = in[x];
				uint32_t r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
				rowcache[x] = 0xff000000u
				            | (((r << 3) | (r >> 2)) << 16)
				            | (((g << 2) | (g >> 4)) << 8)
				            |  ((b << 3) | (b >> 2));
			}
			cached = rowmap[y];
		}
		for (x = 0; x < dw; x++) out[x] = rowcache[colmap[x]];
	}
}

static uint64_t bench2(void (*fn)(uint32_t *, int, int, size_t),
                       uint32_t *dst, int dw, int dh, size_t stride_px, int n)
{
	uint64_t best = ~0ull;
	int i;
	for (i = 0; i < n; i++) {
		uint64_t t = us();
		fn(dst, dw, dh, stride_px);
		t = us() - t;
		if (t < best) best = t;
	}
	return best;
}

static uint64_t bench(uint32_t *dst, int dw, int dh, size_t stride_px, int n)
{
	uint64_t best = ~0ull;
	int i;
	for (i = 0; i < n; i++) {
		uint64_t t = us();
		blit(dst, dw, dh, stride_px);
		t = us() - t;
		if (t < best) best = t;
	}
	return best;
}

int main(void)
{
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	int fd, i, dw, dh;
	uint8_t *fb;
	uint32_t *heap;
	size_t sz;
	uint64_t t_fb, t_heap;

	for (i = 0; i < SRC_W * SRC_H; i++) src[i] = (uint16_t)(i * 2654435761u >> 16);

	fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) { perror("fb0"); return 1; }
	ioctl(fd, FBIOGET_FSCREENINFO, &f);
	ioctl(fd, FBIOGET_VSCREENINFO, &v);
	dw = (int)v.xres; dh = (int)v.yres;
	sz = (size_t)v.yres * f.line_length;
	fb = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) { perror("mmap"); return 2; }

	heap = malloc(sz);
	if (!heap) return 3;
	memset(heap, 0, sz);

	for (i = 0; i < dw; i++) colmap[i] = i * SRC_W / dw;
	for (i = 0; i < dh; i++) rowmap[i] = i * SRC_H / dh;

	printf("%dx%d -> %dx%d, %.2f MB per frame, best of 20\n",
	       SRC_W, SRC_H, dw, dh, (double)dw * dh * 4 / 1048576.0);

	t_fb   = bench((uint32_t *)fb, dw, dh, f.line_length / 4, 20);
	t_heap = bench(heap,           dw, dh, (size_t)dw,        20);

	printf("  -> framebuffer (mmap /dev/fb0) %7.2f ms   %6.0f MB/s\n",
	       t_fb / 1000.0, (double)dw * dh * 4 / (t_fb ? t_fb : 1));
	printf("  -> ordinary heap (malloc)      %7.2f ms   %6.0f MB/s\n",
	       t_heap / 1000.0, (double)dw * dh * 4 / (t_heap ? t_heap : 1));
	{
		uint64_t t_cached = bench2(blit_cached, (uint32_t *)fb, dw, dh,
		                           f.line_length / 4, 20);
		printf("  -> framebuffer, row-cached     %7.2f ms   %6.0f MB/s   %.2fx faster\n",
		       t_cached / 1000.0, (double)dw * dh * 4 / (t_cached ? t_cached : 1),
		       (double)t_fb / (double)t_cached);
	}
	printf("  framebuffer is %.1fx %s\n",
	       t_fb > t_heap ? (double)t_fb / t_heap : (double)t_heap / t_fb,
	       t_fb > t_heap ? "SLOWER - the write is the bottleneck"
	                     : "faster - arithmetic dominates");

	munmap(fb, sz); close(fd); free(heap);
	return 0;
}
