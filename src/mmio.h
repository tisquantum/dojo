/* The console's I/O registers at their own addresses (0x1F801000 on), for the decompiled libraries that
 * drive the hardware directly (LIBSPU, LIBSND). */

#ifndef PORT_MMIO_H
#define PORT_MMIO_H

#include <stdint.h>

/* Maps the I/O page; returns 0 if its address is taken. */
int MmioInit(void);
/* The writable view of the register at a console address: what the game reads there. */
void *MmioRegister(uint32_t address);

#endif
