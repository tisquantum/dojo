/* The interface between the GPU's command processor (gpu.c) and a hardware renderer (vk_renderer.c).
 *
 * The command processor keeps video memory (g_vram) for everything the game uploads, fills and copies. A
 * hardware renderer draws the primitives itself, at a higher resolution, into surfaces of its own; it is
 * told of every change the processor makes to video memory, and asked to bring rendered pixels back into
 * it before the processor reads them. Without one, gpu.c rasterizes into g_vram itself. */

#ifndef PORT_GPU_RENDERER_H
#define PORT_GPU_RENDERER_H

#include <stdint.h>

/* A primitive's corner: position in video memory coordinates (the drawing offset applied), colour, and
 * texture coordinate. */
typedef struct GpuVertex {
    float x, y;
    uint8_t r, g, b, pad;
    float u, v;
} GpuVertex;

/* What a primitive draws with. */
typedef struct GpuDrawState {
    uint8_t textured, raw, semi, semiMode; /* semiMode: 0 B/2+F/2, 1 B+F, 2 B-F, 3 B+F/4 */
    uint8_t texDepth;                      /* 0 4-bit, 1 8-bit, 2 15-bit */
    uint8_t dither, setMask, checkMask;
    int16_t texX, texY;                    /* the texture page's corner */
    int16_t clutX, clutY;
    uint8_t twMaskX, twMaskY, twOffX, twOffY;
    int16_t clipX1, clipY1, clipX2, clipY2; /* the drawing area, inclusive */
} GpuDrawState;

typedef struct GpuRenderer {
    void (*triangle)(const GpuVertex v[3], const GpuDrawState *state);
    /* The command processor filled this rectangle of g_vram with one colour (already in g_vram). */
    void (*fill)(int x, int y, int w, int h, uint32_t rgb);
    /* The command processor changed this rectangle of g_vram. */
    void (*vramWritten)(int x, int y, int w, int h);
    /* The command processor is about to read this rectangle of g_vram. */
    void (*vramRead)(int x, int y, int w, int h);
} GpuRenderer;

void GpuSetRenderer(const GpuRenderer *renderer);
/* All of g_vram was replaced (a save state was loaded). */
void GpuVramRestored(void);

#endif
