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
/* An emulated stack in the simulation's sections (the translated assembly's, tools/mips2c.py), whose words
 * below the stack pointer `*sp` (0: none in use) are dead: before a save or a hash they are cleared, so what
 * was left there (host addresses among it, which differ from machine to machine) doesn't make two machines'
 * identical games hash differently. */
void SaveStateAddDeadStack(void *base, unsigned size, const unsigned *sp);

#endif
