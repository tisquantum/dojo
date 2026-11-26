/* The console's fixed memory as the port holds it (psxport.h): main RAM mapped at the console's address,
 * 0x80000000, which a 32-bit process on a 64-bit system can claim (large-address-aware on Windows), and the
 * scratchpad inside the executable image, aligned so its address can be ORed with an offset as the console's
 * is. */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <stdint.h>
#include "psxport.h"

__attribute__((aligned(4096))) unsigned char g_psxScratch[PSX_SCRATCH_SIZE + 16];

int MemoryInit(void)
{
#ifdef _WIN32
    void *ram = VirtualAlloc((void *)(uintptr_t)PSX_RAM_BASE, PSX_RAM_SIZE, MEM_RESERVE | MEM_COMMIT,
                             PAGE_READWRITE);
#else
    void *ram = mmap((void *)(uintptr_t)PSX_RAM_BASE, PSX_RAM_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
#endif
    return ram == (void *)(uintptr_t)PSX_RAM_BASE;
}
