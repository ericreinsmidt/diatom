/* Persistence - ADR-0016.
 *
 * Host-side entirely. The port deals in pixels, samples, buttons and time, and
 * a file is none of those.
 *
 * Two different things live here and they are not variants of each other:
 *
 *   SRAM is the cartridge battery - the GAME's own save. Automatic and not
 *   optional, because losing it is a defect rather than a missing feature.
 *
 *   A save state is a whole-emulator snapshot the game knows nothing about. A
 *   frontend convenience, so it is opt-in and takes an explicit path.
 *
 * Everything here writes through write_atomic. Measured on the Brick's card:
 * an 8 KB write costs about 8 ms and a 528 KB write about 68 ms, against a
 * 16.6 ms frame budget of which present already takes 8.4 ms. So a save write
 * can NEVER happen on the frame thread; that is what the writer thread is for.
 */
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cheevos.h"
#include "diatom.h"

/* Long enough that a churning game is not writing constantly, short enough
 * that a hard power cut cannot cost much. Graceful shutdown is covered
 * separately: SIGTERM arrives about 810 ms before death (measured), which is
 * ~95x what an 8 KB flush needs. */
#define WRITE_INTERVAL_US 1000000

#define STATE_MAGIC   "DIATOMST"
#define STATE_VERSION 1u

/* Written ahead of the payload so a state from a core that has since changed
 * is refused with a message rather than fed to it. Fixed-width and packed by
 * construction: 8 + 4 + 4 + 64 + 32, no padding on any target. */
struct state_header {
	char     magic[8];
	uint32_t version;
	uint32_t payload;
	char     core_name[64];
	char     core_version[32];
};

static char        g_sram_path[1024];
static uint8_t    *g_sram;        /* the core's buffer; the core owns it */
static uint8_t    *g_shadow;      /* what we last handed to the writer */
static uint8_t    *g_staging;     /* the writer's private copy */
static size_t      g_sram_size;
static uint64_t    g_last_write;

static pthread_t       g_writer;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static bool            g_pending, g_stop, g_running;

static uint64_t now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000ull + t.tv_nsec / 1000;
}

static void logf_(diatom_log_level lvl, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	diatom_port_log(lvl, buf);
}

/* write, fsync, rename. The rename is what makes it atomic: a power cut can
 * lose the new save but can never corrupt the old one. fsync measured free on
 * this filesystem, so there is no argument for skipping it. */
static bool write_atomic(const char *path, const void *data, size_t n)
{
	char tmp[1088];
	int  fd;

	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		logf_(DIATOM_LOG_ERROR, "save: open %s: %s", tmp, strerror(errno));
		return false;
	}
	if (write(fd, data, n) != (ssize_t)n) {
		logf_(DIATOM_LOG_ERROR, "save: write %s: %s", tmp, strerror(errno));
		close(fd);
		unlink(tmp);
		return false;
	}
	fsync(fd);
	close(fd);
	if (rename(tmp, path) != 0) {
		logf_(DIATOM_LOG_ERROR, "save: rename %s: %s", path, strerror(errno));
		unlink(tmp);
		return false;
	}
	return true;
}

static void *writer(void *arg)
{
	pthread_mutex_lock(&g_mx);
	for (;;) {
		while (!g_pending && !g_stop)
			pthread_cond_wait(&g_cv, &g_mx);
		if (g_stop && !g_pending) break;
		g_pending = false;
		pthread_mutex_unlock(&g_mx);

		write_atomic(g_sram_path, g_staging, g_sram_size);

		pthread_mutex_lock(&g_mx);
	}
	pthread_mutex_unlock(&g_mx);
	return arg;
}

/* Basename with the extension stripped. Used only for the standalone
 * defaults; a launcher passes explicit paths and never reaches this. */
static void stem(const char *path, char *out, size_t n)
{
	const char *base = strrchr(path, '/');
	const char *dot;

	base = base ? base + 1 : path;
	dot  = strrchr(base, '.');
	snprintf(out, n, "%.*s", dot ? (int)(dot - base) : (int)strlen(base), base);
}

bool diatom_save_init(diatom_core *c, const char *save_dir, const char *rom_path)
{
	char  name[512];
	FILE *f;

	if (!c->get_memory_data || !c->get_memory_size || !rom_path) return true;

	g_sram_size = c->get_memory_size(RETRO_MEMORY_SAVE_RAM);
	g_sram      = c->get_memory_data(RETRO_MEMORY_SAVE_RAM);

	/* Measured: SRAM size is per GAME, not per system. Probotector reports 0,
	 * Final Fantasy on the same core reports 8 KB. A game with no battery must
	 * produce no file at all rather than an empty one. */
	if (!g_sram_size || !g_sram) {
		logf_(DIATOM_LOG_INFO, "save: no battery in this game");
		return true;
	}

	stem(rom_path, name, sizeof name);
	snprintf(g_sram_path, sizeof g_sram_path, "%s/%s.srm",
	         save_dir ? save_dir : ".", name);

	f = fopen(g_sram_path, "rb");
	if (f) {
		size_t got = fread(g_sram, 1, g_sram_size, f);
		fseek(f, 0, SEEK_END);
		if ((size_t)ftell(f) != g_sram_size)
			/* A core that changed its layout between versions. Load what fits
			 * and say so: a partial load usually still yields a working save,
			 * whereas refusing guarantees the player loses it. */
			logf_(DIATOM_LOG_WARN,
			      "save: %s is %ld bytes, core wants %zu; loaded %zu",
			      g_sram_path, ftell(f), g_sram_size, got);
		else
			logf_(DIATOM_LOG_INFO, "save: loaded %zu bytes from %s",
			      got, g_sram_path);
		fclose(f);
	}

	g_shadow  = malloc(g_sram_size);
	g_staging = malloc(g_sram_size);
	if (!g_shadow || !g_staging) {
		free(g_shadow); free(g_staging);
		g_shadow = g_staging = NULL;
		g_sram_size = 0;
		return false;
	}
	memcpy(g_shadow, g_sram, g_sram_size);
	g_last_write = now_us();

	if (pthread_create(&g_writer, NULL, writer, NULL) == 0)
		g_running = true;
	else
		logf_(DIATOM_LOG_WARN, "save: no writer thread; flushing on exit only");
	return true;
}

void diatom_save_tick(void)
{
	if (!g_sram_size || !g_running) return;
	/* memcmp of at most 128 KB, tens of microseconds. Cheap enough to do every
	 * frame, and most frames find nothing: only games that actually save touch
	 * this. NES carts are the exception - they use battery RAM as work RAM and
	 * change it constantly, which is what the interval is really bounding. */
	if (!memcmp(g_sram, g_shadow, g_sram_size)) return;
	if (now_us() - g_last_write < WRITE_INTERVAL_US) return;

	pthread_mutex_lock(&g_mx);
	memcpy(g_staging, g_sram, g_sram_size);
	memcpy(g_shadow,  g_sram, g_sram_size);
	g_pending = true;
	pthread_cond_signal(&g_cv);
	pthread_mutex_unlock(&g_mx);
	g_last_write = now_us();
}

void diatom_save_flush(void)
{
	if (!g_sram_size) return;
	if (!memcmp(g_sram, g_shadow, g_sram_size)) return;

	/* Synchronous on purpose. This runs on the exit and signal paths, where a
	 * stall costs nothing and a missed write costs the player's progress. */
	if (write_atomic(g_sram_path, g_sram, g_sram_size)) {
		memcpy(g_shadow, g_sram, g_sram_size);
		logf_(DIATOM_LOG_INFO, "save: flushed %zu bytes to %s",
		      g_sram_size, g_sram_path);
	}
}

void diatom_save_shutdown(void)
{
	if (g_running) {
		pthread_mutex_lock(&g_mx);
		g_stop = true;
		pthread_cond_signal(&g_cv);
		pthread_mutex_unlock(&g_mx);
		pthread_join(g_writer, NULL);
		g_running = false;
	}
	diatom_save_flush();
	free(g_shadow); free(g_staging);
	g_shadow = g_staging = NULL;
	g_sram_size = 0;
}

bool diatom_state_save(diatom_core *c, const char *path)
{
	struct state_header h;
	struct retro_system_info si;
	uint8_t *buf;
	size_t   n;
	bool     ok;

	if (!c->serialize_size || !c->serialize || !path) return false;

	/* Called immediately before every serialize, never cached. Measured: mGBA
	 * moved 528448 to 462912 within 600 frames of Golden Sun while reporting
	 * quirks 0x0, having never declared CORE_VARIABLE_SIZE. A cached size that
	 * later grows makes saving fail silently. */
	n = c->serialize_size();
	if (!n) { logf_(DIATOM_LOG_ERROR, "state: core reports no state"); return false; }

	buf = malloc(sizeof h + n);
	if (!buf) return false;

	memset(&h, 0, sizeof h);
	memcpy(h.magic, STATE_MAGIC, sizeof h.magic);
	h.version = STATE_VERSION;
	h.payload = (uint32_t)n;
	c->get_system_info(&si);
	snprintf(h.core_name, sizeof h.core_name, "%s",
	         si.library_name ? si.library_name : "?");
	snprintf(h.core_version, sizeof h.core_version, "%s",
	         si.library_version ? si.library_version : "?");

	if (!c->serialize(buf + sizeof h, n)) {
		logf_(DIATOM_LOG_ERROR, "state: serialize failed");
		free(buf);
		return false;
	}
	memcpy(buf, &h, sizeof h);

	ok = write_atomic(path, buf, sizeof h + n);
	free(buf);
	if (ok) logf_(DIATOM_LOG_INFO, "state: wrote %zu bytes to %s", n, path);
	return ok;
}

bool diatom_state_load(diatom_core *c, const char *path)
{
	struct state_header h;
	struct retro_system_info si;
	uint8_t *buf;
	FILE    *f;
	bool     ok;

	if (!c->unserialize || !path) return false;
	f = fopen(path, "rb");
	if (!f) return false;                     /* absent is not an error */

	if (fread(&h, 1, sizeof h, f) != sizeof h ||
	    memcmp(h.magic, STATE_MAGIC, sizeof h.magic) != 0 ||
	    h.version != STATE_VERSION) {
		logf_(DIATOM_LOG_WARN, "state: %s is not a Diatom state", path);
		fclose(f);
		return false;
	}

	/* A state is only meaningful to the exact core that wrote it. Refusing is
	 * the whole point of the header: the caller falls back to starting the
	 * game normally, which is right, where handing a core a foreign state is
	 * undefined behavior. */
	c->get_system_info(&si);
	if (strcmp(h.core_name, si.library_name ? si.library_name : "?") != 0 ||
	    strcmp(h.core_version, si.library_version ? si.library_version : "?") != 0) {
		logf_(DIATOM_LOG_WARN, "state: %s was written by %s %s, this is %s %s",
		      path, h.core_name, h.core_version,
		      si.library_name ? si.library_name : "?",
		      si.library_version ? si.library_version : "?");
		fclose(f);
		return false;
	}

	buf = malloc(h.payload);
	if (!buf) { fclose(f); return false; }
	if (fread(buf, 1, h.payload, f) != h.payload) {
		logf_(DIATOM_LOG_WARN, "state: %s is truncated", path);
		free(buf); fclose(f);
		return false;
	}
	fclose(f);

	ok = c->unserialize(buf, h.payload);
	free(buf);
	logf_(DIATOM_LOG_INFO, "state: %s %s",
	      ok ? "restored" : "REFUSED by core:", path);

	/* Here rather than at the three call sites. A loaded state is a different
	 * point in the game's history, so half-finished achievement progress -
	 * hit counts, and the previous frame every delta is measured against -
	 * belongs to a timeline that no longer exists. Leaving it would credit
	 * progress nobody made, and a call site added later would inherit the
	 * fix instead of having to remember it. */
	if (ok) diatom_cheevos_runtime_reset();
	return ok;
}
