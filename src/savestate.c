/* Save states (savestate.h). The simulation is:
 * - the data of every unit compiled with sim_sections.h, gathered by the linker between the markers below;
 * - main RAM (0x80000000, 2 MB) and the I/O register page (0x1F801000), mapped by memory.c and mmio.c;
 * - the used part of the game's stack and its saved stack pointer (context.c), so a state saved at any
 *   vertical blank, inside a loading loop as well as at the end of a frame, resumes where it was.
 * Video memory is in the sections (gpu.c); after a restore the renderer is told all of it changed. */

#include <stdio.h>
#include <string.h>
#include "context.h"
#include "gpu.h"
#include "gpu_renderer.h"
#include "mmio.h"
#include "psxport.h"
#include "savestate.h"

/* The markers: COFF orders grouped sections by the text after '$', and "sia" < "sim" < "siz". */
#pragma clang section bss = ".bss$sia" data = ".data$sia"
__attribute__((aligned(16))) static char sDataBegin[16] = {1};
__attribute__((aligned(16))) static char sBssBegin[16];
#pragma clang section bss = ".bss$siz" data = ".data$siz"
__attribute__((aligned(16))) static char sDataEnd[16] = {1};
__attribute__((aligned(16))) static char sBssEnd[16];
#pragma clang section bss = "" data = ""

#define IO_PAGE      0x1F801000u
#define IO_PAGE_SIZE 0x1000u

typedef struct Region {
    uint8_t *base;
    size_t size;
} Region;

/* The simulation's hashed regions, then video memory, saved but not hashed. */
#define HASHED_REGIONS 4

static int Regions(Region out[5])
{
    out[0].base = (uint8_t *)sDataBegin;
    out[0].size = (size_t)((uint8_t *)sDataEnd - (uint8_t *)sDataBegin);
    out[1].base = (uint8_t *)sBssBegin;
    out[1].size = (size_t)((uint8_t *)sBssEnd - (uint8_t *)sBssBegin);
    out[2].base = g_psxRam;
    out[2].size = PSX_RAM_SIZE;
    out[3].base = (uint8_t *)MmioRegister(IO_PAGE);
    out[3].size = IO_PAGE_SIZE;
    out[4].base = (uint8_t *)g_vram;
    out[4].size = sizeof(g_vram);
    return 5;
}

size_t SaveStateCapacity(void)
{
    Region r[5];
    size_t total = 0;
    int i, n = Regions(r);
    void *low, *high;
    for (i = 0; i < n; i++) {
        total += r[i].size;
    }
    ContextGameStack(&low, &high);
    return total + (size_t)((uint8_t *)high - (uint8_t *)low) + 16;
}

static struct {
    uint8_t *base;
    unsigned size;
    const unsigned *sp;
} sDeadStacks[8];
static int sDeadStackCount;

void SaveStateAddDeadStack(void *base, unsigned size, const unsigned *sp)
{
    if (sDeadStackCount < (int)(sizeof(sDeadStacks) / sizeof(sDeadStacks[0]))) {
        sDeadStacks[sDeadStackCount].base = base;
        sDeadStacks[sDeadStackCount].size = size;
        sDeadStacks[sDeadStackCount].sp = sp;
        sDeadStackCount++;
    }
}

static void ClearDeadStacks(void)
{
    int i;
    for (i = 0; i < sDeadStackCount; i++) {
        uintptr_t base = (uintptr_t)sDeadStacks[i].base, sp = *sDeadStacks[i].sp;
        size_t dead = sp > base && sp <= base + sDeadStacks[i].size ? sp - base : sDeadStacks[i].size;
        memset(sDeadStacks[i].base, 0, dead);
    }
}

size_t SaveStateSave(uint8_t *out)
{
    Region r[5];
    int i, n = Regions(r);
    uint8_t *p = out;
    void *sp = ContextGameStackPointer(), *low, *high;
    size_t used;

    ClearDeadStacks();
    for (i = 0; i < n; i++) {
        memcpy(p, r[i].base, r[i].size);
        p += r[i].size;
    }
    ContextGameStack(&low, &high);
    used = (size_t)((uint8_t *)high - (uint8_t *)sp);
    memcpy(p, &sp, sizeof(sp));
    memcpy(p + sizeof(sp), &used, sizeof(used));
    p += 16;
    memcpy(p, sp, used);
    return (size_t)(p + used - out);
}

void SaveStateLoad(const uint8_t *in)
{
    Region r[5];
    int i, n = Regions(r);
    const uint8_t *p = in;
    void *sp;
    size_t used;

    for (i = 0; i < n; i++) {
        memcpy(r[i].base, p, r[i].size);
        p += r[i].size;
    }
    memcpy(&sp, p, sizeof(sp));
    memcpy(&used, p + sizeof(sp), sizeof(used));
    p += 16;
    memcpy(sp, p, used);
    ContextSetGameStackPointer(sp);
    GpuVramRestored();
}

/* FNV-1a over 64-bit words: quick, and enough to tell two states apart. */
static uint64_t HashBytes(uint64_t h, const uint8_t *p, size_t size)
{
    size_t i;
    for (i = 0; i + 8 <= size; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
    }
    for (; i < size; i++) {
        h = (h ^ p[i]) * 0x100000001B3ull;
    }
    return h;
}

uint64_t SaveStateHashState(const uint8_t *state)
{
    Region r[5];
    int i;
    uint64_t h = 0xCBF29CE484222325ull;
    Regions(r);
    for (i = 0; i < HASHED_REGIONS; i++) {
        h = HashBytes(h, state, r[i].size);
        state += r[i].size;
    }
    return h;
}

uint64_t SaveStateHash(void)
{
    Region r[5];
    int i, n = Regions(r);
    uint64_t h = 0xCBF29CE484222325ull;
    (void)n;
    ClearDeadStacks();
    for (i = 0; i < HASHED_REGIONS; i++) {
        h = HashBytes(h, r[i].base, r[i].size);
    }
    return h;
}

void SaveStatePrintRegions(FILE *out)
{
    Region r[5];
    int i, n = Regions(r);
    for (i = 0; i < n; i++) {
        fprintf(out, "region %d %08lx %lu\n", i, (unsigned long)(uintptr_t)r[i].base, (unsigned long)r[i].size);
    }
}
