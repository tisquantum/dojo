/* PsyQ-compatible <rand.h> for the PC port. The console's generator, not the host's: the game's random
 * sequence must match the console's for demos, replays and deterministic netplay. */

#ifndef PORT_RAND_H
#define PORT_RAND_H

#define RAND_MAX 32767

#define rand  PsxRand
#define srand PsxSrand

int PsxRand(void);
void PsxSrand(unsigned int seed);

#endif
