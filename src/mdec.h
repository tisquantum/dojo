/* The MDEC, the movie decoder, behind its registers (mdec.c). */

#ifndef PORT_MDEC_H
#define PORT_MDEC_H

#include <stdint.h>

/* From mmio.c: a write to the MDEC's registers, or to DMA channel 0's or 1's. */
void MdecRegisterWritten(uint32_t address, uint32_t value);
void MdecDmaWritten(uint32_t address);
/* Checks what the translated VLC decoder needs of LIBPRESS's tables; fatal if the build broke it. */
void MdecCheckTables(void);

#endif
