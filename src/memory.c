/* The console's fixed memory as the port holds it (psxport.h): main RAM mapped at the console's address,
 * 0x80000000, which a large-address-aware 32-bit process on 64-bit Windows can claim, and the scratchpad
 * inside the executable image, aligned so its address can be ORed with an offset as the console's is. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "psxport.h"

__declspec(align(4096)) unsigned char g_psxScratch[PSX_SCRATCH_SIZE + 16];

int MemoryInit(void)
{
    void *ram = VirtualAlloc((void *)(uintptr_t)PSX_RAM_BASE, PSX_RAM_SIZE, MEM_RESERVE | MEM_COMMIT,
                             PAGE_READWRITE);
    return ram == (void *)(uintptr_t)PSX_RAM_BASE;
}
