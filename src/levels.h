/* Volume and brightness while a game runs - ADR-0038.
 *
 * The level is the host's: which position each is at, stepping it from the
 * device's level keys with a held key repeating, and setting it from the
 * launcher on its own scale. The port is the hardware under it
 * (include/diatom_port.h). */
#ifndef DIATOM_LEVELS_H
#define DIATOM_LEVELS_H

#include <stdbool.h>

#include "diatom_port.h"

/* Forget every position, and have the port forget what it last wrote: at each
 * handover, when the launcher may have moved either. The next read asks the
 * hardware. */
void diatom_levels_invalidate(void);

/* Where `kind` is, on this device's scale. False if the device has no control
 * over it, or the hardware cannot be read. */
bool diatom_levels_get(diatom_level_kind kind, int *index, int *count);

/* Put `kind` at `index` of `count` positions on the launcher's scale, rescaled
 * to this device's (ADR-0020). False if the device has no control over it. */
bool diatom_levels_set(diatom_level_kind kind, int index, int count);

/* Once a frame: a level key's press steps its level, and holding it repeats at
 * the launcher's pace. */
void diatom_levels_frame(void);

#endif /* DIATOM_LEVELS_H */
