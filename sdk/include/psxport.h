/* The PC port's stand-ins for the console's fixed memory: main RAM, at the console's own address, and the
 * 1 KB scratchpad. Included by every SDK header so game code that names an address gets these.
 *
 * Main RAM is mapped at 0x80000000 (memory.c), where the console's code sees it, so the absolute pointers
 * that files on the disc hold for their fixed load addresses (the motion file at 0x8014C000) work as they
 * are. Ordering-table links keep 24 bits of an address: a link below 0x200000 is in main RAM, and anything
 * else lies in the executable image, which is linked below 16 MB (libgpu.c follows them). */

#ifndef PSXPORT_H
#define PSXPORT_H

#define PSX_RAM_BASE     0x80000000u
#define PSX_RAM_SIZE     0x200000
#define PSX_SCRATCH_SIZE 0x400

#define g_psxRam ((unsigned char *)PSX_RAM_BASE)
extern unsigned char g_psxScratch[]; /* with 16 bytes past it for the debug trap's store (render.s) */

/* A KSEG0 address (0x80xxxxxx) of main RAM, as a pointer: the same address. */
#define PSX_RAM_PTR(addr) ((void *)(PSX_RAM_BASE | ((unsigned long)(addr) & (PSX_RAM_SIZE - 1))))

#define getScratchAddr(offset) ((u_long *)(g_psxScratch + (offset) * 4))

/* Widescreen (host.c), as the simulation sees it: how far past the console's left edge the drawers still
 * draw a face, in console pixels, and how many neighbouring headings' visibility masks the stage cull adds
 * to each side (src/stage_cull.c). Both change what the game writes to memory (the meshes' packets, the
 * render queue), so they are a setting of the simulation, the same on every machine of an online match,
 * not of the display: a display narrower than the setting clips what the extra faces draw. */
extern int g_pcScreenMargin;
extern int g_pcWideHeadings;

/* Maps main RAM at its console address; the host calls it first thing. Returns 0 if the address is taken. */
int MemoryInit(void);

#endif
