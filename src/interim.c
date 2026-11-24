/* Stand-ins for the libraries the port does not run, each doing what lets the game carry on:
 * - LIBCOMB, the link cable, which is never connected. */

#include <libcomb.h>

/* LIBCOMB: no cable. */

void AddCOMB(void) {}
void DelCOMB(void) {}
void ChangeClearSIO(long val) { (void)val; }
long _comb_control(unsigned long cmd, unsigned long arg, unsigned long param)
{
    (void)cmd; (void)arg; (void)param;
    return 0;
}

/* rand and srand are BIOS calls on the console (A0 functions 47 and 48, src/lib/c2); this is the BIOS's
 * linear congruential generator. Its starting seed is not verified against a BIOS. */

static unsigned long sRandNext = 0x24040001;

int PsxRand(void)
{
    sRandNext = sRandNext * 0x41C64E6D + 0x3039;
    return (int)((sRandNext >> 16) & 0x7FFF);
}

void PsxSrand(unsigned int seed)
{
    sRandNext = seed;
}
