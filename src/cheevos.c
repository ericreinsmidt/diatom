/* SPDX-License-Identifier: MIT */
/* Achievements: the address space, and reading from it.
 *
 * See cheevos.h for why this is Diatom's job and not the launcher's, and
 * ADR-0025 for the measurement behind it.
 *
 * A RetroAchievements condition names an address like `0x06f3`. That address
 * is not the core's, and it is not the console's either: it is an offset into
 * a flat space RetroAchievements defines per console, whose layout lives in
 * the vendored rc_consoles.h. The job here is to lay the memory a core has
 * actually offered underneath that space, in order, leaving holes where
 * nothing maps - because an RA address is an offset into the CONCATENATION of
 * the spans, so quietly dropping an unmapped region would shift every address
 * after it and make conditions read the wrong bytes rather than fail.
 *
 * Two sources, in order of preference:
 *
 *   SET_MEMORY_MAPS      a core describing its whole address space, region by
 *                        region. Richer, and the only way to reach anything
 *                        beyond system RAM.
 *   retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM / SAVE_RAM / VIDEO_RAM)
 *                        always available if the core supports achievements at
 *                        all. save.c already uses the SAVE_RAM sibling of this
 *                        call, so the binding exists.
 *
 * Both are captured rather than trusted to persist. libretro's header is
 * explicit that a memory map belongs to the core for the duration of the call
 * only - "the frontend must maintain its own copy of this object and its
 * contents" - and a core re-declares its map on every retro_load_game, so a
 * pointer held across a load points into memory that core has freed.
 *
 * PROVENANCE. The descriptor decoding and the region-splitting below follow
 * rcheevos' own src/rc_libretro.c, which is MIT like the rest of it and like
 * Diatom. ADR-0025 records why that one file is not vendored - it drags in
 * rhash's disc and archive readers for playlist hashing Diatom will never do -
 * but not vendoring it is not a license to guess. The console tables it reads
 * ARE vendored and are used here unmodified; what is rewritten is the plumbing
 * between them and libretro's descriptors, and it is rewritten to match. The
 * bit-collapsing loop in desc_for() is upstream's, which took it in turn from
 * RetroArch's mmap_reduce.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cheevos.h"
#include "diatom.h"

#include "rc_consoles.h"
#include "rc_error.h"
#include "rc_runtime.h"
#include "rc_runtime_types.h"

/* ---------------------------------------------------------------- capture */

/* Our own copy of the descriptors. Only the fields the decoding reads are
 * kept; the address-space names are not, because nothing here shows them to
 * anyone and copying them would be storage with no reader. `offset` is folded
 * into `ptr` on the way in, which is what every use of it would have done. */
typedef struct {
	uint8_t *ptr;
	size_t   start;
	size_t   select;
	size_t   disconnect;
	size_t   len;
} kept_descriptor;

static kept_descriptor g_desc[DIATOM_MEM_DESCS];
static int             g_ndesc;

static diatom_mem_span g_span[DIATOM_MEM_SPANS];
static int             g_nspan;
static size_t          g_total;      /* RA address space covered, holes included */
static size_t          g_mapped;     /* of which readable */

static unsigned g_console;
static bool     g_supported = true;  /* until a core says otherwise */
static bool     g_support_stated;    /* ...and whether it ever said anything */

/* Kept so a map arriving AFTER the load can be acted on. Most cores declare
 * theirs inside retro_load_game, but the API does not require it and some send
 * one on a region or cartridge change, which is exactly when the old pointers
 * stop being right. */
static diatom_core *g_core;

void diatom_cheevos_reset(void)
{
	memset(g_desc, 0, sizeof g_desc);
	memset(g_span, 0, sizeof g_span);
	g_ndesc = 0;
	g_nspan = 0;
	g_total = 0;
	g_mapped = 0;
	g_supported = true;
	g_support_stated = false;
	g_core = NULL;
	diatom_cheevos_unload();
	/* g_console is NOT cleared: it is the launcher's statement about the
	 * content it is about to ask for, and it arrives before the load. */
}

void diatom_cheevos_set_console(unsigned ra_console_id)
{
	g_console = ra_console_id;
}

void diatom_cheevos_note_support(bool supported)
{
	g_supported = supported;
	g_support_stated = true;
}

static void   report(void);
static size_t resolve_now(diatom_core *c);

void diatom_cheevos_note_map(const void *mmap_v)
{
	const struct retro_memory_map *m = mmap_v;
	unsigned i;

	g_ndesc = 0;
	if (!m || !m->descriptors) return;

	for (i = 0; i < m->num_descriptors && g_ndesc < DIATOM_MEM_DESCS; i++) {
		const struct retro_memory_descriptor *d = &m->descriptors[i];

		/* A descriptor with no pointer describes an address range that maps
		 * to nothing readable - a mirror of something already claimed, or
		 * hardware. It is kept anyway, pointer and all: it still CLAIMS those
		 * addresses, and dropping it would let a later descriptor answer for
		 * them. The mapping turns it into a hole. */
		g_desc[g_ndesc].ptr = d->ptr ? (uint8_t *)d->ptr + d->offset : NULL;
		g_desc[g_ndesc].start      = d->start;
		g_desc[g_ndesc].select     = d->select;
		g_desc[g_ndesc].disconnect = d->disconnect;
		g_desc[g_ndesc].len        = d->len;
		g_ndesc++;
	}

	if (i < m->num_descriptors)
		diatom_port_log(DIATOM_LOG_WARN,
		                "cheevos: core declared more memory regions than we keep");

	/* A map that lands after the load replaces the one the load produced.
	 * Silent unless the answer actually changed, because a core is free to
	 * re-send the same map and a log line per frame is not information. */
	if (g_core) {
		const size_t before = g_mapped;

		resolve_now(g_core);
		if (g_mapped != before) report();
	}
}

/* ---------------------------------------------------------------- mapping */

static void span_add(uint8_t *data, size_t size)
{
	if (size == 0) return;

	/* Merge with the previous span when the two are one run of host memory,
	 * or both holes. The NES alone is eleven console regions over three
	 * descriptors; without merging, the interesting cores would spend the
	 * span budget on bookkeeping. */
	if (g_nspan > 0) {
		diatom_mem_span *last = &g_span[g_nspan - 1];

		if ((!data && !last->data) ||
		    (data && last->data && data == last->data + last->size)) {
			last->size += size;
			g_total    += size;
			if (data) g_mapped += size;
			return;
		}
	}

	if (g_nspan == DIATOM_MEM_SPANS) {
		diatom_port_log(DIATOM_LOG_WARN,
		                "cheevos: out of memory spans; the tail of the address "
		                "space is unreadable");
		return;
	}

	g_span[g_nspan].data = data;
	g_span[g_nspan].size = size;
	g_nspan++;
	g_total += size;
	if (data) g_mapped += size;
}

/* Which descriptor claims a console address, and where in it the address
 * lands. libretro's rule: `select` says which bits of the address must match
 * `start`, and `disconnect` says which bits are not wired to the memory chip
 * at all and have to be squeezed out. First match wins, per the header. */
static const kept_descriptor *desc_for(uint32_t real_address, size_t *offset)
{
	int i;

	for (i = 0; i < g_ndesc; i++) {
		const kept_descriptor *d = &g_desc[i];
		uint32_t reduced, disconnect;

		if (d->select == 0) {
			/* No select: start and len are the whole mapping, each byte
			 * once. */
			if (real_address >= d->start && real_address < d->start + d->len) {
				*offset = real_address - d->start;
				return d;
			}
			continue;
		}

		if (((uint32_t)d->start ^ real_address) & (uint32_t)d->select) continue;

		reduced    = real_address - (uint32_t)d->start;
		disconnect = (uint32_t)d->disconnect;

		/* Remove the disconnected bits and close the gaps they leave. From
		 * RetroArch's mmap_reduce by way of rcheevos. */
		while (disconnect) {
			const uint32_t low = (disconnect - 1) & ~disconnect;

			reduced    = (reduced & low) | ((reduced >> 1) & ~low);
			disconnect = (disconnect & (disconnect - 1)) >> 1;
		}

		/* A descriptor can match the select bits and still be too small to
		 * hold the address; that is not this one, so keep looking. */
		if (reduced < d->len) {
			*offset = reduced;
			return d;
		}
	}

	*offset = 0;
	return NULL;
}

static void map_from_descriptors(const rc_memory_regions_t *regions)
{
	uint32_t i;

	for (i = 0; i < regions->num_regions; i++) {
		const rc_memory_region_t *r = &regions->region[i];
		size_t   remaining = (size_t)(r->end_address - r->start_address) + 1;
		uint32_t real      = r->real_address;
		size_t   flip      = 0;

		while (remaining > 0) {
			const kept_descriptor *d;
			size_t   offset = 0, run;
			uint8_t *at;

			d = desc_for(real, &offset);
			if (!d) {
				/* The gap may only last until the next time the lowest
				 * disconnected bit flips, and what follows may well be
				 * mapped. Skip that far rather than abandoning the region. */
				if (flip && remaining > flip) {
					span_add(NULL, flip);
					remaining -= flip;
					real      += (uint32_t)flip;
					flip       = 0;
					continue;
				}
				span_add(NULL, remaining);
				break;
			}

			at  = d->ptr ? d->ptr + offset : NULL;
			run = d->len - offset;

			if (d->disconnect && run > d->disconnect) {
				/* The longest run we can read straight through ends where the
				 * lowest disconnected bit next changes. */
				flip = d->disconnect & (~d->disconnect + 1);
				run  = flip - (real & (flip - 1));
			}

			if (run == 0) {          /* no progress possible; stop guessing */
				span_add(NULL, remaining);
				break;
			}

			if (run > remaining) run = remaining;
			span_add(at, run);
			remaining -= run;
			real      += (uint32_t)run;
		}
	}
}

static unsigned retro_type_for(uint8_t rc_type)
{
	switch (rc_type) {
	case RC_MEMORY_TYPE_SAVE_RAM:  return RETRO_MEMORY_SAVE_RAM;
	case RC_MEMORY_TYPE_VIDEO_RAM: return RETRO_MEMORY_VIDEO_RAM;
	default:                       return RETRO_MEMORY_SYSTEM_RAM;
	}
}

static void core_memory(diatom_core *c, unsigned retro_type,
                        uint8_t **data, size_t *size)
{
	*data = NULL;
	*size = 0;
	if (!c || !c->get_memory_data || !c->get_memory_size) return;

	*data = c->get_memory_data(retro_type);
	*size = *data ? c->get_memory_size(retro_type) : 0;
	if (!*size) *data = NULL;
}

/* No memory map: the console's regions have to be found by offsetting into
 * whatever retro_get_memory_data hands back for each kind of memory. */
static void map_from_core(diatom_core *c, const rc_memory_regions_t *regions)
{
	uint32_t i, j;
	bool disjoint = false;

	for (i = 0; i < regions->num_regions; i++) {
		const rc_memory_region_t *r  = &regions->region[i];
		const size_t             want = (size_t)(r->end_address - r->start_address) + 1;
		const unsigned           rt   = retro_type_for(r->type);
		uint32_t base = 0;
		uint8_t *data;
		size_t   size, offset;

		/* A 64KB-or-larger hole in an address space bigger than 16MB is
		 * padding put there to keep real addresses aligned, which means the
		 * console's memory is not one run and cannot be reached by offsetting
		 * into system RAM. Stop resolving; the rest becomes holes. */
		if (!disjoint && r->type == RC_MEMORY_TYPE_UNUSED && want >= 0x10000 &&
		    regions->region[regions->num_regions - 1].end_address > 0x01000000)
			disjoint = true;

		/* Where this kind of memory starts in the console's space, so the
		 * offset is measured into the core's block and not into the map. */
		for (j = 0; j <= i; j++) {
			if (retro_type_for(regions->region[j].type) == rt) {
				base = regions->region[j].start_address;
				break;
			}
		}
		offset = r->start_address - base;

		if (disjoint) {
			data = NULL;
			size = want;
		} else {
			core_memory(c, rt, &data, &size);
		}

		if (offset < size) {
			size -= offset;
			if (data) data += offset;
		} else {
			data = NULL;
			size = 0;
		}

		if (want > size) {
			span_add(data, size);
			span_add(NULL, want - size);
		} else {
			span_add(data, want);
		}
	}
}

/* No console table either. rcheevos assumes system RAM then save RAM in that
 * case, and so does this: it is the layout the simplest sets are written
 * against, and being wrong here is no worse than mapping nothing. */
static void map_without_regions(diatom_core *c)
{
	uint8_t *data;
	size_t   size;

	core_memory(c, RETRO_MEMORY_SYSTEM_RAM, &data, &size);
	span_add(data, size);
	core_memory(c, RETRO_MEMORY_SAVE_RAM, &data, &size);
	span_add(data, size);
}

static size_t resolve_now(diatom_core *c)
{
	const rc_memory_regions_t *regions = rc_console_memory_regions(g_console);

	memset(g_span, 0, sizeof g_span);
	g_nspan  = 0;
	g_total  = 0;
	g_mapped = 0;

	if (!regions || regions->num_regions == 0)
		map_without_regions(c);
	else if (g_ndesc > 0)
		map_from_descriptors(regions);
	else
		map_from_core(c, regions);

	return g_mapped;
}

static void report(void)
{
	char msg[192];

	snprintf(msg, sizeof msg,
	         "cheevos: %s via %s: %zu of %zu bytes readable, %d spans%s",
	         rc_console_name(g_console),
	         g_ndesc ? "the core's memory map" : "retro_get_memory_data",
	         g_mapped, g_total, g_nspan,
	         g_supported ? "" : " (core says achievements are unsupported)");

	/* A core that offered nothing and claimed nothing is the ordinary case for
	 * a core that is not an emulator, and not worth a warning. A core that
	 * declared a map, or said in so many words that it supports achievements,
	 * and still resolves to nothing is a real problem: every condition would
	 * read zero, and some conditions are true when their address is zero. */
	diatom_port_log(g_mapped == 0 && (g_ndesc > 0 ||
	                                  (g_support_stated && g_supported))
	                    ? DIATOM_LOG_WARN : DIATOM_LOG_INFO,
	                msg);
}

size_t diatom_cheevos_resolve(diatom_core *c)
{
	g_core = c;
	resolve_now(c);
	report();
	return g_mapped;
}

/* ---------------------------------------------------------------- reading */

uint32_t diatom_cheevos_peek(uint32_t address, uint32_t num_bytes, void *ud)
{
	uint8_t  buf[4];
	uint32_t got = 0;
	int      i;

	(void)ud;
	if (num_bytes == 0 || num_bytes > sizeof buf) return 0;

	for (i = 0; i < g_nspan && got < num_bytes; i++) {
		const size_t size = g_span[i].size;
		size_t       run;

		if (address >= size) {          /* not here; the space is contiguous */
			address -= (uint32_t)size;
			continue;
		}
		if (!g_span[i].data) return 0;  /* a hole: refuse rather than invent */

		run = size - address;
		if (run > num_bytes - got) run = num_bytes - got;
		memcpy(buf + got, g_span[i].data + address, run);
		got    += (uint32_t)run;
		address = 0;
	}

	/* All of it or none. A short read padded with zeroes is a value, and a
	 * value is something a condition can compare against and fire on. */
	if (got != num_bytes) return 0;

	switch (num_bytes) {
	case 1:  return buf[0];
	case 2:  return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8);
	case 3:  return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
	                ((uint32_t)buf[2] << 16);
	default: return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
	                ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
	}
}

/* ------------------------------------------------- the set, and the frames */

/* The evaluator. Everything about a condition - deltas, hit counts, alt
 * groups, AndNext chains - is rcheevos' problem, which is the whole reason
 * ADR-0025 vendored it. Diatom's side is memory and a clock. */
static rc_runtime_t g_rt;
static bool         g_rt_live;

static const char *state_name(uint8_t state)
{
	switch (state) {
	case RC_TRIGGER_STATE_TRIGGERED: return "unlocked";
	case RC_TRIGGER_STATE_PRIMED:    return "primed";
	case RC_TRIGGER_STATE_PAUSED:    return "paused";
	case RC_TRIGGER_STATE_DISABLED:  return "disabled";
	default:                         return "active";
	}
}

/* Two events are reported, and the choice is deliberate.
 *
 * TRIGGERED is the feature. DISABLED means the achievement referenced memory
 * that is not there, which without a report is indistinguishable from one the
 * player has not earned - the silent failure this whole file is arranged to
 * avoid.
 *
 * PRIMED and UNPRIMED are not reported yet. They would let a launcher show
 * "one condition away", but they are edge events on game state and can arrive
 * often. Adding an event later costs nothing, because ADR-0009 promises
 * unknown verbs are ignored; removing chatter from a launcher that came to
 * depend on it costs a great deal. */
static void on_event(const rc_runtime_event_t *e)
{
	char msg[96];

	switch (e->type) {
	case RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED:
		diatom_proto_send("CHEEVO\tid=%u\tstate=unlocked", (unsigned)e->id);
		snprintf(msg, sizeof msg, "cheevos: %u unlocked", (unsigned)e->id);
		diatom_port_log(DIATOM_LOG_INFO, msg);
		break;
	case RC_RUNTIME_EVENT_ACHIEVEMENT_DISABLED:
		diatom_proto_send("CHEEVO\tid=%u\tstate=disabled", (unsigned)e->id);
		snprintf(msg, sizeof msg,
		         "cheevos: %u disabled; it reads memory this core does not map",
		         (unsigned)e->id);
		diatom_port_log(DIATOM_LOG_WARN, msg);
		break;
	default:
		break;
	}
}

void diatom_cheevos_unload(void)
{
	if (g_rt_live) {
		rc_runtime_destroy(&g_rt);
		g_rt_live = false;
	}
}

int diatom_cheevos_load(const char *path)
{
	/* getline, not a fixed buffer, and this is measured rather than cautious.
	 * Across 428 achievements from four RetroAchievements sets on 2026-08-29
	 * the median condition string is 113 characters and the longest is 30,897
	 * - Mega Man 2. Any buffer chosen by intuition would have been too small,
	 * and the failure would have been an achievement that quietly never fires,
	 * on the hardest achievements in the hardest games. */
	char  *line = NULL;
	size_t cap  = 0;
	char   msg[256];
	FILE  *f;
	int    ok = 0, bad = 0;

	diatom_cheevos_unload();
	if (!path || !*path) return 0;

	f = fopen(path, "r");
	if (!f) {
		snprintf(msg, sizeof msg, "cheevos: cannot read the set at %s", path);
		diatom_port_log(DIATOM_LOG_WARN, msg);
		return 0;
	}

	rc_runtime_init(&g_rt);
	g_rt_live = true;

	while (getline(&line, &cap, f) > 0) {
		unsigned long id;
		char *tab, *end;
		int rc;

		line[strcspn(line, "\r\n")] = '\0';
		if (!line[0] || line[0] == '#') continue;

		tab = strchr(line, '\t');
		if (!tab) { bad++; continue; }
		*tab++ = '\0';

		id = strtoul(line, &end, 10);
		if (end == line || *end || id == 0) { bad++; continue; }

		/* Anything past a second tab is room the format leaves for a title
		 * or points later, and is not read here. */
		end = strchr(tab, '\t');
		if (end) *end = '\0';

		rc = rc_runtime_activate_achievement(&g_rt, (uint32_t)id, tab, NULL, 0);
		if (rc == RC_OK) {
			ok++;
		} else {
			bad++;
			snprintf(msg, sizeof msg, "cheevos: %lu rejected: %s",
			         id, rc_error_str(rc));
			diatom_port_log(DIATOM_LOG_WARN, msg);
		}
	}
	free(line);
	fclose(f);

	snprintf(msg, sizeof msg, "cheevos: %d watched, %d rejected, from %s",
	         ok, bad, path);
	diatom_port_log(bad ? DIATOM_LOG_WARN : DIATOM_LOG_INFO, msg);

	/* A set with nothing readable underneath it evaluates every condition
	 * against zero, and some conditions are true when their address is zero.
	 * Say so rather than firing achievements at a player who did nothing. */
	if (ok && g_mapped == 0)
		diatom_port_log(DIATOM_LOG_WARN,
		                "cheevos: a set is loaded but no memory resolved; "
		                "nothing will be evaluated");
	return ok;
}

void diatom_cheevos_frame(void)
{
	/* Both guards matter. Without a set there is nothing to do; without
	 * memory, every peek returns zero and the runtime would happily conclude
	 * things about a console that is not there. */
	if (!g_rt_live || g_mapped == 0) return;

	rc_runtime_do_frame(&g_rt, on_event, diatom_cheevos_peek, NULL, NULL);
}

void diatom_cheevos_runtime_reset(void)
{
	if (g_rt_live) rc_runtime_reset(&g_rt);
}

void diatom_cheevos_emit(void)
{
	uint32_t i, n = g_rt_live ? g_rt.trigger_count : 0, unlocked = 0;

	for (i = 0; i < n; i++) {
		const rc_trigger_t *t = g_rt.triggers[i].trigger;

		if (t && t->state == RC_TRIGGER_STATE_TRIGGERED) unlocked++;
	}

	diatom_proto_send("CHEEVOS\tconsole=%u\tcount=%u\tunlocked=%u"
	                  "\tmemory=%zu\tspans=%d",
	                  g_console, (unsigned)n, (unsigned)unlocked,
	                  g_mapped, g_nspan);

	for (i = 0; i < n; i++) {
		const rc_runtime_trigger_t *rt = &g_rt.triggers[i];

		diatom_proto_send("CHEEVO\tid=%u\tstate=%s", (unsigned)rt->id,
		                  rt->trigger ? state_name(rt->trigger->state)
		                              : "disabled");
	}
}

/* ---------------------------------------------------------------- reports */

bool diatom_cheevos_supported(void)
{
	/* A core that never said is not a core that said no. The header makes the
	 * frontend's discretion explicit for exactly this case: "if
	 * retro_get_memory_data returns a valid address but this environment call
	 * is not used, the frontend may or may not opt in the core". Diatom opts
	 * in, because having readable memory is the whole requirement. */
	return g_supported && g_mapped > 0;
}

size_t diatom_cheevos_mapped_bytes(void) { return g_mapped; }
size_t diatom_cheevos_total_bytes(void)  { return g_total; }
int    diatom_cheevos_span_count(void)   { return g_nspan; }

const diatom_mem_span *diatom_cheevos_span(int i)
{
	return (i >= 0 && i < g_nspan) ? &g_span[i] : NULL;
}
