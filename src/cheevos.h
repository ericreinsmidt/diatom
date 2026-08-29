/* SPDX-License-Identifier: MIT */
#ifndef DIATOM_CHEEVOS_H
#define DIATOM_CHEEVOS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* For diatom_core. It is a typedef of an anonymous struct, so there is no
 * `struct diatom_core` tag to forward-declare - the header has to come in. */
#include "diatom.h"

/* Achievements: turning what the core offers into the flat address space
 * RetroAchievements writes its conditions against.
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
 * Three things have to line up before a condition can be read:
 *
 *   the console      RetroAchievements defines a separate address space per
 *                    console, so `0x06f3` means nothing until you know it is
 *                    an NES. Diatom never guesses this; the launcher knows it
 *                    and says so.
 *   the core's map   SET_MEMORY_MAPS, if the core sends one.
 *   the fallback     retro_get_memory_data, for the cores that do not.
 */

/* One run of the RetroAchievements address space that resolves to real memory,
 * or to nothing. Holes are kept rather than skipped, because RA addresses are
 * offsets into the concatenation of these spans - dropping an unmapped one
 * would silently shift every address after it. */
typedef struct {
	uint8_t *data;         /* NULL: this run of addresses is not readable */
	size_t   size;
} diatom_mem_span;

#define DIATOM_MEM_SPANS 64    /* NES needs 11 console regions split by mirror */
#define DIATOM_MEM_DESCS 32    /* the widest map in the pinned set has 10 */

/* Reset to "this core has offered nothing", called before each load. A core
 * declares its map during retro_load_game, so anything held from the previous
 * game is not merely stale - it points into memory that core has freed. */
void diatom_cheevos_reset(void);

/* Which console's address space to resolve into, as a RetroAchievements
 * console id (RC_CONSOLE_* in rc_consoles.h). Zero, the default, means unknown:
 * the mapping then assumes system RAM followed by save RAM, which is what
 * rcheevos itself does for a console it has no table for. */
void diatom_cheevos_set_console(unsigned ra_console_id);

/* SET_MEMORY_MAPS. The header is explicit that the frontend must keep its own
 * copy of the descriptors and everything they point at, because the core's
 * copy is only valid for the duration of the call. */
void diatom_cheevos_note_map(const void *retro_memory_map);

/* SET_SUPPORT_ACHIEVEMENTS. A core saying false means it knows its memory is
 * not stable enough to be worth watching, and is worth honoring. */
void diatom_cheevos_note_support(bool supported);

/* Build the address space from whatever the core offered. Called once the game
 * is loaded, since neither source is complete before that. Takes the core
 * rather than reaching for a global, the same way diatom_env_bind does.
 * Returns the number of readable bytes, which is zero when nothing mapped. */
size_t diatom_cheevos_resolve(diatom_core *c);

/* Read up to four bytes at a RetroAchievements address, little-endian.
 * Signature-compatible with rc_runtime_peek_t; cheevos.c proves that at
 * compile time rather than trusting this comment. A read that runs off the end
 * of mapped memory returns 0 whole, never a partial value - a half-read that
 * looks like data is how a condition fires on a game that is not running. */
uint32_t diatom_cheevos_peek(uint32_t address, uint32_t num_bytes, void *ud);

/* ---- the set, and evaluating it -----------------------------------------
 *
 * ADR-0026. The launcher owns the network and the account; Diatom owns the
 * frame. So the launcher fetches a set, writes it to a file and says where it
 * is, and Diatom evaluates it against every frame the core produces.
 *
 * The file is one achievement per line, tab separated:
 *
 *     <id>\t<condition string>[\t anything else, ignored]
 *
 * Blank lines and lines starting with '#' are skipped. It is not JSON, on
 * purpose: RetroAchievements' wire format is the launcher's problem, and a
 * parser in here would be a second place that has to track their schema.
 *
 * The launcher sends only what it wants watched. An achievement the player has
 * already earned is simply left out of the file - Diatom has no account, no
 * idea what "earned" means, and nowhere to keep it. */

/* Read a set and activate it. Replaces whatever was loaded. A NULL or empty
 * path unloads. Returns the number of achievements now being watched. */
int  diatom_cheevos_load(const char *path);
void diatom_cheevos_unload(void);

/* One frame of evaluation, called after retro_run and before anything can
 * change memory again. Emits CHEEVO to the launcher as things fire. Cheap and
 * safe to call when nothing is loaded. */
void diatom_cheevos_frame(void);

/* Forget hit counts and deltas without forgetting the set. A loaded state is
 * a different point in the game's history, and carrying a half-finished match
 * across it would credit progress that did not happen. */
void diatom_cheevos_runtime_reset(void);

/* The state plane's query reply (ADR-0020): a CHEEVOS summary followed by one
 * CHEEVO line per achievement. */
void diatom_cheevos_emit(void);

/* What was found, for the log and for the launcher to be told about. */
bool   diatom_cheevos_supported(void);
size_t diatom_cheevos_mapped_bytes(void);   /* readable */
size_t diatom_cheevos_total_bytes(void);    /* readable + holes */
int    diatom_cheevos_span_count(void);
const diatom_mem_span *diatom_cheevos_span(int i);

#endif
