/* The port's GPU: video memory and a software rasterizer of the console GPU's GP0 commands. It draws what
 * the console draws: 15-bit colour, texture pages of 4, 8 and 15 bits through colour tables, the texture
 * window, the four semi-transparency modes, dithering, the mask bit, and the drawing area and offset. */

#include <string.h>
#include "gpu.h"
#include "gpu_renderer.h"
#include "gte_soft.h"

/* Video memory is saved with the simulation but kept out of its hash (savestate.c): a renderer writes what it
 * drew back into it, which depends on the machine's display settings. */
#pragma clang section bss = ".bss$vram"
uint16_t g_vram[VRAM_HEIGHT][VRAM_WIDTH];
#pragma clang section bss = ".bss$sim"
GpuDisplay g_gpuDisplay;

static struct {
    int texX, texY;     /* texture page base */
    int semiMode;       /* 0 B/2+F/2, 1 B+F, 2 B-F, 3 B+F/4 */
    int texDepth;       /* 0 4-bit, 1 8-bit, 2 15-bit */
    int dither;
    int twMaskX, twMaskY, twOffX, twOffY;
    int clipX1, clipY1, clipX2, clipY2;
    int offX, offY;
    int setMask, checkMask;
} sGpu;

static const int sDither[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

void GpuReset(void)
{
    memset(&sGpu, 0, sizeof(sGpu));
    sGpu.clipX2 = VRAM_WIDTH - 1;
    sGpu.clipY2 = VRAM_HEIGHT - 1;
    g_gpuDisplay.width = 640;
    g_gpuDisplay.height = 240;
}

static void SetTexPage(uint32_t bits)
{
    sGpu.texX = (bits & 0xF) * 64;
    sGpu.texY = ((bits >> 4) & 1) * 256;
    sGpu.semiMode = (bits >> 5) & 3;
    sGpu.texDepth = (bits >> 7) & 3;
    if (sGpu.texDepth == 3) {
        sGpu.texDepth = 2;
    }
    sGpu.dither = (bits >> 9) & 1;
}

static int SignExtend11(uint32_t v)
{
    return (int)(v << 21) >> 21;
}

/* One texel of the current page, through the colour table at (clutX, clutY) for 4 and 8 bits. */
static uint16_t FetchTexel(int u, int v, int clutX, int clutY)
{
    u = (u & ~sGpu.twMaskX) | (sGpu.twOffX & sGpu.twMaskX);
    v = (v & ~sGpu.twMaskY) | (sGpu.twOffY & sGpu.twMaskY);
    u &= 0xFF;
    v &= 0xFF;
    switch (sGpu.texDepth) {
    case 0: {
        uint16_t word = g_vram[(sGpu.texY + v) & 511][(sGpu.texX + u / 4) & 1023];
        int index = (word >> ((u & 3) * 4)) & 0xF;
        return g_vram[clutY & 511][(clutX + index) & 1023];
    }
    case 1: {
        uint16_t word = g_vram[(sGpu.texY + v) & 511][(sGpu.texX + u / 2) & 1023];
        int index = (word >> ((u & 1) * 8)) & 0xFF;
        return g_vram[clutY & 511][(clutX + index) & 1023];
    }
    default:
        return g_vram[(sGpu.texY + v) & 511][(sGpu.texX + u) & 1023];
    }
}

static int Clamp5(int c)
{
    return c < 0 ? 0 : (c > 31 ? 31 : c);
}

/* Writes one pixel: r, g, b are 8-bit vertex colours (or 5-bit texel colours when `texel15`), blended. */
static void PlotPixel(int x, int y, uint16_t color, int semi, int forceMaskBit)
{
    uint16_t *dst = &g_vram[y][x];
    if (sGpu.checkMask && (*dst & 0x8000)) {
        return;
    }
    if (semi) {
        int br = *dst & 31, bg = (*dst >> 5) & 31, bb = (*dst >> 10) & 31;
        int fr = color & 31, fg = (color >> 5) & 31, fb = (color >> 10) & 31;
        int r, g, b;
        switch (sGpu.semiMode) {
        case 0:
            r = (br + fr) >> 1, g = (bg + fg) >> 1, b = (bb + fb) >> 1;
            break;
        case 1:
            r = br + fr, g = bg + fg, b = bb + fb;
            break;
        case 2:
            r = br - fr, g = bg - fg, b = bb - fb;
            break;
        default:
            r = br + (fr >> 2), g = bg + (fg >> 2), b = bb + (fb >> 2);
            break;
        }
        color = (uint16_t)(Clamp5(r) | Clamp5(g) << 5 | Clamp5(b) << 10 | (color & 0x8000));
    }
    *dst = (uint16_t)(color | (sGpu.setMask || forceMaskBit ? 0x8000 : 0));
}

/* The colour of an untextured pixel from 8-bit components, dithered when asked. */
static uint16_t ShadeColor(int r, int g, int b, int x, int y, int dither)
{
    if (dither) {
        int d = sDither[y & 3][x & 3];
        r += d, g += d, b += d;
    }
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return (uint16_t)((r >> 3) | (g >> 3) << 5 | (b >> 3) << 10);
}

/* A textured pixel: the texel's colour times the vertex colour / 128, or the texel alone when raw. */
static uint16_t BlendTexel(uint16_t texel, int r, int g, int b, int raw, int x, int y, int dither)
{
    int tr, tg, tb;
    if (raw) {
        return texel;
    }
    tr = (texel & 31) * r;
    tg = ((texel >> 5) & 31) * g;
    tb = ((texel >> 10) & 31) * b;
    /* 5-bit texel times 8-bit colour / 128 is the 8-bit result * 8 / 128; work in 8 bits to dither. */
    return (uint16_t)(ShadeColor(tr >> 4, tg >> 4, tb >> 4, x, y, dither) | (texel & 0x8000));
}

typedef struct Vertex {
    int x, y;
    float fx, fy; /* the sub-pixel position, for a hardware renderer */
    int r, g, b;
    int u, v;
} Vertex;

typedef struct PrimState {
    int textured, raw, semi, gouraud;
    int clutX, clutY;
} PrimState;

/* Which renderer draws is the machine's setting, not the simulation's. */
#pragma clang section bss = ".bss$render" data = ".data$render"
static const GpuRenderer *sRenderer;
unsigned g_gpuDrawCount;
#pragma clang section bss = ".bss$sim" data = ".data$sim"

void GpuSetRenderer(const GpuRenderer *renderer)
{
    sRenderer = renderer;
}

void GpuVramRestored(void)
{
    if (sRenderer) {
        sRenderer->vramWritten(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    }
}

static void HwDrawState(GpuDrawState *s, const PrimState *ps)
{
    memset(s, 0, sizeof(*s));
    s->textured = (uint8_t)ps->textured;
    s->raw = (uint8_t)ps->raw;
    s->semi = (uint8_t)ps->semi;
    s->semiMode = (uint8_t)sGpu.semiMode;
    s->texDepth = (uint8_t)sGpu.texDepth;
    s->dither = (uint8_t)sGpu.dither;
    s->setMask = (uint8_t)sGpu.setMask;
    s->checkMask = (uint8_t)sGpu.checkMask;
    s->texX = (int16_t)sGpu.texX;
    s->texY = (int16_t)sGpu.texY;
    s->clutX = (int16_t)ps->clutX;
    s->clutY = (int16_t)ps->clutY;
    s->twMaskX = (uint8_t)sGpu.twMaskX;
    s->twMaskY = (uint8_t)sGpu.twMaskY;
    s->twOffX = (uint8_t)sGpu.twOffX;
    s->twOffY = (uint8_t)sGpu.twOffY;
    s->clipX1 = (int16_t)sGpu.clipX1;
    s->clipY1 = (int16_t)sGpu.clipY1;
    s->clipX2 = (int16_t)sGpu.clipX2;
    s->clipY2 = (int16_t)sGpu.clipY2;
}

static GpuVertex HwVertex(float x, float y, int r, int g, int b, float u, float v)
{
    GpuVertex out;
    out.x = x;
    out.y = y;
    out.r = (uint8_t)r;
    out.g = (uint8_t)g;
    out.b = (uint8_t)b;
    out.pad = 0;
    out.u = u;
    out.v = v;
    return out;
}

static void HwTriangle(const Vertex *a, const Vertex *b, const Vertex *c, const PrimState *ps)
{
    GpuDrawState state;
    GpuVertex v[3];
    int maxX = a->x > b->x ? (a->x > c->x ? a->x : c->x) : (b->x > c->x ? b->x : c->x);
    int minX = a->x < b->x ? (a->x < c->x ? a->x : c->x) : (b->x < c->x ? b->x : c->x);
    int maxY = a->y > b->y ? (a->y > c->y ? a->y : c->y) : (b->y > c->y ? b->y : c->y);
    int minY = a->y < b->y ? (a->y < c->y ? a->y : c->y) : (b->y < c->y ? b->y : c->y);
    /* The GPU skips polygons wider than 1023 or taller than 511 pixels. */
    if (maxX - minX >= 1024 || maxY - minY >= 512) {
        return;
    }
    HwDrawState(&state, ps);
    v[0] = HwVertex(a->fx, a->fy, a->r, a->g, a->b, (float)a->u, (float)a->v);
    v[1] = HwVertex(b->fx, b->fy, b->r, b->g, b->b, (float)b->u, (float)b->v);
    v[2] = HwVertex(c->fx, c->fy, c->r, c->g, c->b, (float)c->u, (float)c->v);
    sRenderer->triangle(v, &state);
}

/* A rectangle or a line as two triangles, for the hardware renderer. */
static void HwQuad(const GpuVertex q[4], const PrimState *ps)
{
    GpuDrawState state;
    GpuVertex t[3];
    HwDrawState(&state, ps);
    t[0] = q[0], t[1] = q[1], t[2] = q[2];
    sRenderer->triangle(t, &state);
    t[0] = q[1], t[1] = q[2], t[2] = q[3];
    sRenderer->triangle(t, &state);
}

static int64_t Edge(const Vertex *a, const Vertex *b, int x, int y)
{
    return (int64_t)(b->x - a->x) * (y - a->y) - (int64_t)(b->y - a->y) * (x - a->x);
}

/* The top-left fill rule: pixels on a top or left edge are drawn, on a bottom or right edge not. */
static int IsTopLeft(const Vertex *a, const Vertex *b)
{
    return (a->y == b->y && b->x < a->x) || (b->y < a->y);
}

static void DrawTriangle(Vertex v0, Vertex v1, Vertex v2, const PrimState *ps)
{
    int minX, maxX, minY, maxY, x, y;
    int64_t area;
    int bias0, bias1, bias2;
    int dither = sGpu.dither && (ps->gouraud || (ps->textured && !ps->raw));

    area = Edge(&v0, &v1, v2.x, v2.y);
    if (area == 0) {
        return;
    }
    if (area < 0) {
        Vertex t = v1;
        v1 = v2;
        v2 = t;
        area = -area;
    }
    minX = v0.x < v1.x ? (v0.x < v2.x ? v0.x : v2.x) : (v1.x < v2.x ? v1.x : v2.x);
    maxX = v0.x > v1.x ? (v0.x > v2.x ? v0.x : v2.x) : (v1.x > v2.x ? v1.x : v2.x);
    minY = v0.y < v1.y ? (v0.y < v2.y ? v0.y : v2.y) : (v1.y < v2.y ? v1.y : v2.y);
    maxY = v0.y > v1.y ? (v0.y > v2.y ? v0.y : v2.y) : (v1.y > v2.y ? v1.y : v2.y);
    /* The GPU skips polygons wider than 1023 or taller than 511 pixels. */
    if (maxX - minX >= 1024 || maxY - minY >= 512) {
        return;
    }
    if (minX < sGpu.clipX1) minX = sGpu.clipX1;
    if (minY < sGpu.clipY1) minY = sGpu.clipY1;
    if (maxX > sGpu.clipX2) maxX = sGpu.clipX2;
    if (maxY > sGpu.clipY2) maxY = sGpu.clipY2;

    bias0 = IsTopLeft(&v1, &v2) ? 0 : -1;
    bias1 = IsTopLeft(&v2, &v0) ? 0 : -1;
    bias2 = IsTopLeft(&v0, &v1) ? 0 : -1;

    for (y = minY; y <= maxY; y++) {
        for (x = minX; x <= maxX; x++) {
            int64_t w0 = Edge(&v1, &v2, x, y);
            int64_t w1 = Edge(&v2, &v0, x, y);
            int64_t w2 = Edge(&v0, &v1, x, y);
            int r, g, b;
            uint16_t color;
            if (w0 + bias0 < 0 || w1 + bias1 < 0 || w2 + bias2 < 0) {
                continue;
            }
            if (ps->gouraud) {
                r = (int)((w0 * v0.r + w1 * v1.r + w2 * v2.r) / area);
                g = (int)((w0 * v0.g + w1 * v1.g + w2 * v2.g) / area);
                b = (int)((w0 * v0.b + w1 * v1.b + w2 * v2.b) / area);
            } else {
                r = v0.r, g = v0.g, b = v0.b;
            }
            if (ps->textured) {
                int u = (int)((w0 * v0.u + w1 * v1.u + w2 * v2.u) / area);
                int v = (int)((w0 * v0.v + w1 * v1.v + w2 * v2.v) / area);
                uint16_t texel = FetchTexel(u, v, ps->clutX, ps->clutY);
                if (texel == 0) {
                    continue;
                }
                color = BlendTexel(texel, r, g, b, ps->raw, x, y, dither);
                PlotPixel(x, y, color, ps->semi && (texel & 0x8000), 0);
            } else {
                color = ShadeColor(r, g, b, x, y, dither);
                PlotPixel(x, y, color, ps->semi, 0);
            }
        }
    }
}

static void DrawRect(int x0, int y0, int w, int h, int r, int g, int b, int u0, int v0, const PrimState *ps)
{
    int x, y;
    if (sRenderer) {
        GpuVertex q[4];
        if (ps->raw) {
            r = g = b = 128;
        }
        q[0] = HwVertex((float)x0, (float)y0, r, g, b, (float)u0, (float)v0);
        q[1] = HwVertex((float)(x0 + w), (float)y0, r, g, b, (float)(u0 + w), (float)v0);
        q[2] = HwVertex((float)x0, (float)(y0 + h), r, g, b, (float)u0, (float)(v0 + h));
        q[3] = HwVertex((float)(x0 + w), (float)(y0 + h), r, g, b, (float)(u0 + w), (float)(v0 + h));
        HwQuad(q, ps);
        return;
    }
    for (y = 0; y < h; y++) {
        int py = y0 + y;
        if (py < sGpu.clipY1 || py > sGpu.clipY2) {
            continue;
        }
        for (x = 0; x < w; x++) {
            int px = x0 + x;
            if (px < sGpu.clipX1 || px > sGpu.clipX2) {
                continue;
            }
            if (ps->textured) {
                uint16_t texel = FetchTexel(u0 + x, v0 + y, ps->clutX, ps->clutY);
                if (texel == 0) {
                    continue;
                }
                PlotPixel(px, py, BlendTexel(texel, r, g, b, ps->raw, px, py, 0), ps->semi && (texel & 0x8000), 0);
            } else {
                PlotPixel(px, py, ShadeColor(r, g, b, px, py, 0), ps->semi, 0);
            }
        }
    }
}

static void DrawLine(Vertex a, Vertex b, const PrimState *ps)
{
    if (sRenderer) {
        /* A one-pixel-thick quad along the line's major axis. */
        GpuVertex q[4];
        int dx = b.x - a.x, dy = b.y - a.y;
        float alongX = 0, alongY = 0, acrossX = 0, acrossY = 0;
        if ((dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy)) {
            alongX = 1, acrossY = 1; /* x-major: the end pixel included, one pixel tall */
        } else {
            alongY = 1, acrossX = 1;
        }
        q[0] = HwVertex((float)a.x, (float)a.y, a.r, a.g, a.b, 0, 0);
        q[1] = HwVertex((float)b.x + alongX, (float)b.y + alongY, b.r, b.g, b.b, 0, 0);
        q[2] = HwVertex((float)a.x + acrossX, (float)a.y + acrossY, a.r, a.g, a.b, 0, 0);
        q[3] = HwVertex((float)b.x + alongX + acrossX, (float)b.y + alongY + acrossY, b.r, b.g, b.b, 0, 0);
        HwQuad(q, ps);
        return;
    }
    int dx = b.x - a.x, dy = b.y - a.y;
    int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    int i;
    int dither = sGpu.dither && ps->gouraud;
    if (steps >= 1024) {
        return;
    }
    for (i = 0; i <= steps; i++) {
        int x = steps ? a.x + (dx * i + (dx >= 0 ? steps / 2 : -steps / 2)) / steps : a.x;
        int y = steps ? a.y + (dy * i + (dy >= 0 ? steps / 2 : -steps / 2)) / steps : a.y;
        int r = ps->gouraud && steps ? a.r + (b.r - a.r) * i / steps : a.r;
        int g = ps->gouraud && steps ? a.g + (b.g - a.g) * i / steps : a.g;
        int bl = ps->gouraud && steps ? a.b + (b.b - a.b) * i / steps : a.b;
        if (x < sGpu.clipX1 || x > sGpu.clipX2 || y < sGpu.clipY1 || y > sGpu.clipY2) {
            continue;
        }
        PlotPixel(x, y, ShadeColor(r, g, bl, x, y, dither), ps->semi, 0);
    }
}

static void ReadColor(Vertex *v, uint32_t word)
{
    v->r = word & 0xFF;
    v->g = (word >> 8) & 0xFF;
    v->b = (word >> 16) & 0xFF;
}

static void ReadPosition(Vertex *v, uint32_t word)
{
    float px, py;
    v->x = SignExtend11(word) + sGpu.offX;
    v->y = SignExtend11(word >> 16) + sGpu.offY;
    if (sRenderer && GtePreciseLookup(word, &px, &py)) {
        v->fx = px + (float)sGpu.offX;
        v->fy = py + (float)sGpu.offY;
    } else {
        v->fx = (float)v->x;
        v->fy = (float)v->y;
    }
}

int GpuCommandLength(uint32_t word)
{
    uint32_t code = word >> 24;
    if (code >= 0x20 && code < 0x40) {
        int vertices = (code & 0x08) ? 4 : 3;
        int perVertex = 1 + ((code & 0x04) ? 1 : 0) + ((code & 0x10) ? 1 : 0);
        return 1 + vertices * perVertex - ((code & 0x10) ? 1 : 0);
    }
    if (code >= 0x40 && code < 0x60) {
        return (code & 0x08) ? 0 : ((code & 0x10) ? 4 : 3);
    }
    if (code >= 0x60 && code < 0x80) {
        return 2 + ((code & 0x04) ? 1 : 0) + (((code >> 3) & 3) == 0 ? 1 : 0);
    }
    switch (code) {
    case 0x02:
        return 3;
    case 0x80:
        return 4;
    case 0xA0:
    case 0xC0:
        return 3;
    default:
        return 1;
    }
}

static void DrawPolygon(const uint32_t *w)
{
    uint32_t code = w[0] >> 24;
    PrimState ps;
    Vertex v[4];
    int count = (code & 0x08) ? 4 : 3;
    int i, p = 0;

    memset(&ps, 0, sizeof(ps));
    memset(v, 0, sizeof(v));
    ps.gouraud = (code & 0x10) != 0;
    ps.textured = (code & 0x04) != 0;
    ps.semi = (code & 0x02) != 0;
    ps.raw = ps.textured && (code & 0x01);
    for (i = 0; i < count; i++) {
        if (i == 0 || ps.gouraud) {
            ReadColor(&v[i], w[p++]);
        } else {
            v[i].r = v[0].r, v[i].g = v[0].g, v[i].b = v[0].b;
        }
        ReadPosition(&v[i], w[p++]);
        if (ps.textured) {
            uint32_t t = w[p++];
            v[i].u = t & 0xFF;
            v[i].v = (t >> 8) & 0xFF;
            if (i == 0) {
                ps.clutX = ((t >> 16) & 0x3F) * 16;
                ps.clutY = (t >> 22) & 0x1FF;
            } else if (i == 1) {
                SetTexPage(t >> 16);
            }
        }
    }
    if (ps.raw) {
        for (i = 0; i < count; i++) {
            v[i].r = v[i].g = v[i].b = 128;
        }
    }
    if (sRenderer) {
        HwTriangle(&v[0], &v[1], &v[2], &ps);
        if (count == 4) {
            HwTriangle(&v[1], &v[2], &v[3], &ps);
        }
        return;
    }
    DrawTriangle(v[0], v[1], v[2], &ps);
    if (count == 4) {
        DrawTriangle(v[1], v[2], v[3], &ps);
    }
}

void GpuCommand(const uint32_t *w, int count)
{
    uint32_t code = w[0] >> 24;
    (void)count;
    g_gpuDrawCount++;

    if (code >= 0x20 && code < 0x40) {
        DrawPolygon(w);
        return;
    }
    if (code >= 0x40 && code < 0x60) {
        PrimState ps;
        Vertex a, b;
        memset(&ps, 0, sizeof(ps));
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        ps.gouraud = (code & 0x10) != 0;
        ps.semi = (code & 0x02) != 0;
        if (code & 0x08) {
            /* A polyline: vertices until the 0x5xxx5xxx terminator. */
            int p = 1;
            ReadColor(&a, w[0]);
            ReadPosition(&a, w[p++]);
            for (;;) {
                if (ps.gouraud) {
                    if ((w[p] & 0xF000F000u) == 0x50005000u) break;
                    ReadColor(&b, w[p++]);
                } else {
                    b.r = a.r, b.g = a.g, b.b = a.b;
                }
                if ((w[p] & 0xF000F000u) == 0x50005000u) break;
                ReadPosition(&b, w[p++]);
                DrawLine(a, b, &ps);
                a = b;
            }
            return;
        }
        ReadColor(&a, w[0]);
        ReadPosition(&a, w[1]);
        if (ps.gouraud) {
            ReadColor(&b, w[2]);
            ReadPosition(&b, w[3]);
        } else {
            b.r = a.r, b.g = a.g, b.b = a.b;
            ReadPosition(&b, w[2]);
        }
        DrawLine(a, b, &ps);
        return;
    }
    if (code >= 0x60 && code < 0x80) {
        PrimState ps;
        Vertex a;
        int p = 2, wdt, hgt, u = 0, v = 0;
        static const int sizes[4] = {0, 1, 8, 16};
        memset(&ps, 0, sizeof(ps));
        ps.textured = (code & 0x04) != 0;
        ps.semi = (code & 0x02) != 0;
        ps.raw = ps.textured && (code & 0x01);
        ReadColor(&a, w[0]);
        ReadPosition(&a, w[1]);
        if (ps.textured) {
            uint32_t t = w[p++];
            u = t & 0xFF;
            v = (t >> 8) & 0xFF;
            ps.clutX = ((t >> 16) & 0x3F) * 16;
            ps.clutY = (t >> 22) & 0x1FF;
        }
        if (((code >> 3) & 3) == 0) {
            wdt = w[p] & 0x3FF;
            hgt = (w[p] >> 16) & 0x1FF;
        } else {
            wdt = hgt = sizes[(code >> 3) & 3];
        }
        DrawRect(a.x, a.y, wdt, hgt, a.r, a.g, a.b, u, v, &ps);
        return;
    }
    switch (code) {
    case 0x02:
        GpuFillRect(w[1] & 0x3F0, (w[1] >> 16) & 0x1FF, ((w[2] & 0x3FF) + 15) & ~15, (w[2] >> 16) & 0x1FF,
                    w[0] & 0xFFFFFF);
        break;
    case 0x80:
        GpuCopyVram(w[1] & 0x3FF, (w[1] >> 16) & 0x1FF, w[2] & 0x3FF, (w[2] >> 16) & 0x1FF, w[3] & 0xFFFF,
                    w[3] >> 16);
        break;
    case 0xE1:
        SetTexPage(w[0]);
        break;
    case 0xE2:
        sGpu.twMaskX = (w[0] & 0x1F) * 8;
        sGpu.twMaskY = ((w[0] >> 5) & 0x1F) * 8;
        sGpu.twOffX = ((w[0] >> 10) & 0x1F) * 8;
        sGpu.twOffY = ((w[0] >> 15) & 0x1F) * 8;
        break;
    case 0xE3:
        sGpu.clipX1 = w[0] & 0x3FF;
        sGpu.clipY1 = (w[0] >> 10) & 0x1FF;
        break;
    case 0xE4:
        sGpu.clipX2 = w[0] & 0x3FF;
        sGpu.clipY2 = (w[0] >> 10) & 0x1FF;
        break;
    case 0xE5:
        sGpu.offX = SignExtend11(w[0]);
        sGpu.offY = SignExtend11(w[0] >> 11);
        break;
    case 0xE6:
        sGpu.setMask = w[0] & 1;
        sGpu.checkMask = (w[0] >> 1) & 1;
        break;
    default:
        break;
    }
}

void GpuFillRect(int x, int y, int w, int h, uint32_t rgb)
{
    g_gpuDrawCount++;
    uint16_t c = (uint16_t)(((rgb >> 3) & 31) | ((rgb >> 11) & 31) << 5 | ((rgb >> 19) & 31) << 10);
    int i, j;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            g_vram[(y + j) & 511][(x + i) & 1023] = c;
        }
    }
    if (sRenderer) {
        sRenderer->fill(x, y, w, h, rgb);
    }
}

void GpuWriteVram(int x, int y, int w, int h, const uint16_t *src)
{
    g_gpuDrawCount++;
    int i, j;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            uint16_t *dst = &g_vram[(y + j) & 511][(x + i) & 1023];
            if (sGpu.checkMask && (*dst & 0x8000)) {
                src++;
                continue;
            }
            *dst = (uint16_t)(*src++ | (sGpu.setMask ? 0x8000 : 0));
        }
    }
    if (sRenderer) {
        sRenderer->vramWritten(x, y, w, h);
    }
}

void GpuReadVram(int x, int y, int w, int h, uint16_t *dst)
{
    int i, j;
    if (sRenderer) {
        sRenderer->vramRead(x, y, w, h);
    }
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            *dst++ = g_vram[(y + j) & 511][(x + i) & 1023];
        }
    }
}

void GpuCopyVram(int sx, int sy, int dx, int dy, int w, int h)
{
    int i, j;
    if (w == 0) w = 1024;
    if (h == 0) h = 512;
    if (sRenderer) {
        sRenderer->vramRead(sx, sy, w, h);
    }
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            uint16_t c = g_vram[(sy + j) & 511][(sx + i) & 1023];
            uint16_t *dst = &g_vram[(dy + j) & 511][(dx + i) & 1023];
            if (sGpu.checkMask && (*dst & 0x8000)) {
                continue;
            }
            *dst = (uint16_t)(c | (sGpu.setMask ? 0x8000 : 0));
        }
    }
    if (sRenderer) {
        sRenderer->vramWritten(dx, dy, w, h);
    }
}
