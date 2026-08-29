/* SPDX-License-Identifier: MIT */
/* Achievements: capturing what the core offers.
 *
 * See cheevos.h for why this is Diatom's job and not the launcher's, and
 * ADR-0025 for the measurement behind it.
 *
 * Two sources, in order of preference:
 *
 *   SET_MEMORY_MAPS      a core describing its whole address space, region by
 *                        region. Richer, and the only way to reach anything
 *                        beyond system RAM.
 *   retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM)
 *                        one block, always available if the core supports
 *                        achievements at all. save.c already uses the SAVE_RAM
 *                        sibling of this call, so the binding exists.
 *
 * Both are captured rather than trusted to persist. libretro's header is
 * explicit that a memory map belongs to the core for the duration of the call
 * only - "the frontend must maintain its own copy of this object and its
 * contents" - and a core re-declares its map on every retro_load_game, so a
 * pointer held across a load points into memory that core has freed.
 *
 * What is deliberately NOT here: any judgement about what an address means.
 * RetroAchievements' own console tables in rc_consoles.h say which of its
 * addresses correspond to which region, and re-deriving that here would be the
 * exact mistake ADR-0025 vendors rcheevos to avoid.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cheevos.h"
#include "diatom.h"

static diatom_mem_block g_block[DIATOM_MEM_BLOCKS];
static int              g_nblocks;
static bool             g_supported = true;   /* until a core says otherwise */
static bool             g_support_stated;

/* Our own copy of the descriptors. Only the fields Diatom reads are kept; the
 * strings are not, because nothing here shows them to anyone and copying them
 * would be storage with no reader. */
typedef struct {
	uint8_t *ptr;
	size_t   len;
	uint32_t start;
	uint32_t flags;
} kept_descriptor;

static kept_descriptor g_desc[DIATOM_MEM_BLOCKS];
static int             g_ndesc;

void diatom_cheevos_reset(void)
{
	memset(g_block, 0, sizeof g_block);
	memset(g_desc, 0, sizeof g_desc);
	g_nblocks = 0;
	g_ndesc = 0;
	g_supported = true;
	g_support_stated = false;
}

void diatom_cheevos_note_support(bool supported)
{
	g_supported = supported;
	g_support_stated = true;
}

void diatom_cheevos_note_map(const void *mmap_v)
{
	const struct retro_memory_map *m = mmap_v;
	unsigned i;

	g_ndesc = 0;
	if (!m || !m->descriptors) return;

	for (i = 0; i < m->num_descriptors && g_ndesc < DIATOM_MEM_BLOCKS; i++) {
		const struct retro_memory_descriptor *d = &m->descriptors[i];

		/* A descriptor with no pointer describes an address range that maps to
		 * nothing readable - a mirror, or hardware. Keeping it would produce a
		 * block that reads as zeroes and looks like RAM that is always empty,
		 * which is worse than not having it. */
		if (!d->ptr || !d->len) continue;

		g_desc[g_ndesc].ptr   = (uint8_t *)d->ptr + d->offset;
		g_desc[g_ndesc].len   = d->len;
		g_desc[g_ndesc].start = (uint32_t)d->start;
		g_desc[g_ndesc].flags = (uint32_t)d->flags;
		g_ndesc++;
	}

	if (i < m->num_descriptors)
		diatom_port_log(DIATOM_LOG_WARN,
		                "cheevos: core declared more memory regions than we keep");
}

int diatom_cheevos_resolve(diatom_core *c)
{
	int i;

	g_nblocks = 0;

	for (i = 0; i < g_ndesc && g_nblocks < DIATOM_MEM_BLOCKS; i++) {
		g_block[g_nblocks].data     = g_desc[i].ptr;
		g_block[g_nblocks].size     = g_desc[i].len;
		g_block[g_nblocks].start    = g_desc[i].start;
		g_block[g_nblocks].from_map = true;
		g_nblocks++;
	}

	/* Every core in the test matrix answers this, and several answer nothing
	 * else: a map is optional and system RAM is not. */
	if (g_nblocks == 0 && c && c->get_memory_data && c->get_memory_size) {
		void  *p = c->get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
		size_t n = c->get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
		if (p && n) {
			g_block[0].data     = p;
			g_block[0].size     = n;
			g_block[0].start    = 0;
			g_block[0].from_map = false;
			g_nblocks = 1;
		}
	}
	return g_nblocks;
}

int diatom_cheevos_block_count(void) { return g_nblocks; }

const diatom_mem_block *diatom_cheevos_block(int i)
{
	return (i >= 0 && i < g_nblocks) ? &g_block[i] : NULL;
}

bool diatom_cheevos_supported(void)
{
	/* A core that never said is not a core that said no. The header makes the
	 * frontend's discretion explicit for exactly this case: "if
	 * retro_get_memory_data returns a valid address but this environment call
	 * is not used, the frontend may or may not opt in the core". Diatom opts
	 * in, because having readable memory is the whole requirement. */
	return g_supported && g_nblocks > 0;
}

size_t diatom_cheevos_total_bytes(void)
{
	size_t n = 0;
	int i;
	for (i = 0; i < g_nblocks; i++) n += g_block[i].size;
	return n;
}
