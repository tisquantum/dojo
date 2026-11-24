/* The port's GPU: the console's 1 MB of video memory and its command processor, drawing as the console's GPU
 * does. LIBGPU (libgpu.c) feeds it packets; the host shows its display area. */

#ifndef PORT_GPU_H
#define PORT_GPU_H

#include <stdint.h>

#define VRAM_WIDTH  1024
#define VRAM_HEIGHT 512

extern uint16_t g_vram[VRAM_HEIGHT][VRAM_WIDTH];

typedef struct GpuDisplay {
    int x, y;          /* top-left of the shown area in VRAM */
    int width, height; /* its size in VRAM pixels */
    int enabled;       /* SetDispMask */
    int rgb24;
} GpuDisplay;

extern GpuDisplay g_gpuDisplay;

void GpuReset(void);
/* Runs one GP0 command of `words` words. */
void GpuCommand(const uint32_t *words, int count);
/* The number of words of the GP0 command starting with `word` (0 for one GpuCommand cannot take whole). */
int GpuCommandLength(uint32_t word);
void GpuFillRect(int x, int y, int w, int h, uint32_t rgb);
void GpuWriteVram(int x, int y, int w, int h, const uint16_t *src);
void GpuReadVram(int x, int y, int w, int h, uint16_t *dst);
void GpuCopyVram(int sx, int sy, int dx, int dy, int w, int h);

#endif
