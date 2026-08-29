/* SPDX-License-Identifier: MIT */
#ifndef DIATOM_CHEEVOS_H
#define DIATOM_CHEEVOS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* For diatom_core. It is a typedef of an anonymous struct, so there is no
 * `struct diatom_core` tag to forward-declare - the header has to come in. */
#include "diatom.h"

/* Achievements.
 *
 * This lives in Diatom rather than in the launcher because libretro says so:
 * RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS is addressed to the FRONTEND, and
 * the core's only obligation is to expose its address space. The measured
 * reason is in ADR-0025 - conditions compare against the previous frame, and
 * the launcher only sees the socket every 100ms against a 60Hz core, so six
 * frames in seven would be invisible to it.
 *
 * Diatom evaluates. The launcher does the network: it logs in, identifies the
 * game and hands over a condition set. Nothing here opens a socket.
 *
 * This first piece only captures what the core offers. Evaluation comes next.
 */

/* One usable block of emulated memory, already resolved to a pointer Diatom
 * can read. */
typedef struct {
	uint8_t *data;
	size_t   size;
	uint32_t start;        /* where the core says this block begins */
	bool     from_map;     /* from SET_MEMORY_MAPS, else retro_get_memory_data */
} diatom_mem_block;

#define DIATOM_MEM_BLOCKS 16

/* Reset to "this core has offered nothing", called before each load. A core
 * declares its map during retro_load_game, so anything held from the previous
 * game is not merely stale - it points into memory that core has freed. */
void diatom_cheevos_reset(void);

/* SET_MEMORY_MAPS. The header is explicit that the frontend must keep its own
 * copy of the descriptors and everything they point at, because the core's
 * copy is only valid for the duration of the call. */
void diatom_cheevos_note_map(const void *retro_memory_map);

/* SET_SUPPORT_ACHIEVEMENTS. A core saying false means it knows its memory is
 * not stable enough to be worth watching, and is worth honouring. */
void diatom_cheevos_note_support(bool supported);

/* Fill in the blocks from whatever the core offered, preferring the memory map
 * and falling back to retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM). Called
 * once the game is loaded, since neither source is complete before that.
 * Takes the core rather than reaching for a global, the same way
 * diatom_env_bind does. Returns the number of blocks found. */
int diatom_cheevos_resolve(diatom_core *c);

/* What was found, for the log and for the launcher to be told about. */
int  diatom_cheevos_block_count(void);
const diatom_mem_block *diatom_cheevos_block(int i);
bool diatom_cheevos_supported(void);
size_t diatom_cheevos_total_bytes(void);

#endif
