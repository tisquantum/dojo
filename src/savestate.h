/* Save states: the whole simulation, saved and restored while the game is suspended at a vertical blank. */

#ifndef PORT_SAVESTATE_H
#define PORT_SAVESTATE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* The most bytes a save state takes. */
size_t SaveStateCapacity(void);
/* Saves the simulation into `out` (SaveStateCapacity bytes); returns the bytes used. */
size_t SaveStateSave(uint8_t *out);
void SaveStateLoad(const uint8_t *in);
/* A hash of the simulation as it is now, for comparing two machines' or two runs' states. */
uint64_t SaveStateHash(void);
/* The same hash of a saved state. */
uint64_t SaveStateHashState(const uint8_t *state);
/* Prints the regions a save state holds, in order, with their addresses (for debugging). */
void SaveStatePrintRegions(FILE *out);

#endif
