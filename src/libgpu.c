/* LIBGPU's system layer for the port: what the console library does through the GPU's ports and DMA, done
 * on the port's GPU (gpu.c). Drawing is immediate, so DrawSync never waits. The packet-building routines
 * produce the words the library's own do (src/lib/gpu/sys.c, for the standard GPU); the rest of LIBGPU
 * (src/lib/gpu/prim.c, ext.c) is the decompiled library itself. */

#include <stdio.h>
#include <string.h>
#include <libgpu.h>
#include "gpu.h"
#include "gte_soft.h"

#define GP0_DRAW_MODE    0xE1000000u
#define GP0_TEX_WINDOW   0xE2000000u
#define GP0_DRAW_AREA_TL 0xE3000000u
#define GP0_DRAW_AREA_BR 0xE4000000u
#define GP0_DRAW_OFFSET  0xE5000000u
#define GP0_MASK_BIT     0xE6000000u
#define GP0_TILE         0x60000000u
#define GP0_FILL_RECT    0x02000000u
#define OT_END           0xFFFFFFu

static struct {
    DRAWENV draw;
    DISPENV disp;
    int debug;
    void (*drawSyncCallback)(void);
} sEnv;

int (*GPU_printf)() = (int (*)())printf;

static u_long sTerm[5] = {0x04FFFFFFu, 0, 0, 0, 0};

#define LIMIT(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

/* Runs a packet's words as a stream of GP0 commands. */
static void RunWords(const uint32_t *w, int len)
{
    int i = 0;
    while (i < len) {
        uint32_t code = w[i] >> 24;
        int n = GpuCommandLength(w[i]);
        if (n == 0) {
            /* A polyline runs to its terminator. */
            n = 2;
            while (i + n < len && (w[i + n] & 0xF000F000u) != 0x50005000u) {
                n++;
            }
            n++;
        }
        if (code == 0xA0 && i + 3 <= len) {
            int x = w[i + 1] & 0x3FF, y = (w[i + 1] >> 16) & 0x1FF;
            int wd = w[i + 2] & 0xFFFF, ht = w[i + 2] >> 16;
            int words = (wd * ht + 1) / 2;
            GpuWriteVram(x, y, wd, ht, (const uint16_t *)&w[i + 3]);
            i += 3 + words;
            continue;
        }
        if (i + n > len) {
            break;
        }
        GpuCommand(&w[i], n);
        i += n;
    }
}

void ResetGraph(int mode)
{
    if (mode == 0 || mode == 3) {
        GpuReset();
    }
    memset(&sEnv.draw, 0, sizeof(sEnv.draw));
    memset(&sEnv.disp, 0, sizeof(sEnv.disp));
}

int SetGraphDebug(int level)
{
    int old = sEnv.debug;
    sEnv.debug = level;
    return old;
}

int GetGraphType(void)
{
    return 0;
}

void SetDispMask(int mask)
{
    g_gpuDisplay.enabled = mask != 0;
}

int DrawSync(int mode)
{
    (void)mode;
    return 0;
}

u_long DrawSyncCallback(void (*func)(void))
{
    void (*old)(void) = sEnv.drawSyncCallback;
    sEnv.drawSyncCallback = func;
    return (u_long)old;
}

static void DrawDone(void)
{
    if (sEnv.drawSyncCallback) {
        sEnv.drawSyncCallback();
    }
}

int ClearImage(RECT *rect, u_char r, u_char g, u_char b)
{
    GpuFillRect(rect->x & 0x3F0, rect->y, (rect->w + 15) & ~15, rect->h, r | g << 8 | b << 16);
    DrawDone();
    return 0;
}

int ClearImage2(RECT *rect, u_char r, u_char g, u_char b)
{
    return ClearImage(rect, r, g, b);
}

int LoadImage(RECT *rect, u_long *p)
{
    GpuWriteVram(rect->x, rect->y, rect->w, rect->h, (const uint16_t *)p);
    DrawDone();
    return 0;
}

int StoreImage(RECT *rect, u_long *p)
{
    GpuReadVram(rect->x, rect->y, rect->w, rect->h, (uint16_t *)p);
    DrawDone();
    return 0;
}

int MoveImage(RECT *rect, int x, int y)
{
    GpuCopyVram(rect->x, rect->y, x, y, rect->w, rect->h);
    DrawDone();
    return 0;
}

u_long *ClearOTag(u_long *ot, int n)
{
    int i;
    for (i = 0; i < n - 1; i++) {
        ot[i] = (u_long)&ot[i + 1] & OT_END;
    }
    ot[n - 1] = (u_long)sTerm & OT_END;
    return ot;
}

u_long *ClearOTagR(u_long *ot, int n)
{
    int i;
    for (i = n - 1; i > 0; i--) {
        ot[i] = (u_long)&ot[i - 1] & OT_END;
    }
    ot[0] = (u_long)sTerm & OT_END;
    return ot;
}

void DrawOTag(u_long *p)
{
    unsigned guard = 0;
    while (p != NULL) {
        uint32_t tag = (uint32_t)*p;
        uint32_t next = tag & OT_END;
        int len = (int)(tag >> 24);
        if (len) {
            RunWords((const uint32_t *)(p + 1), len);
        }
        if (next == OT_END || ++guard > 0x100000) {
            break;
        }
        /* A link below 0x200000 is in main RAM, mapped at its console address (psxport.h). */
        p = (u_long *)(uintptr_t)(next < PSX_RAM_SIZE ? (PSX_RAM_BASE | next) : next);
    }
    GtePreciseNewFrame(); /* the frame's packets are drawn: its projections are done with */
    DrawDone();
}

void DrawPrim(void *p)
{
    RunWords((const uint32_t *)p + 1, (int)(*(uint32_t *)p >> 24));
}

static u_long GetModeWord(int dfe, int dtd, u_short tpage)
{
    return GP0_DRAW_MODE | (dtd ? 0x200 : 0) | (dfe ? 0x400 : 0) | (tpage & 0x9FF);
}

static u_long GetClipStart(short x, short y)
{
    x = LIMIT(x, 0, VRAM_WIDTH - 1);
    y = LIMIT(y, 0, VRAM_HEIGHT - 1);
    return GP0_DRAW_AREA_TL | (y & 0x3FF) << 10 | (x & 0x3FF);
}

static u_long GetClipEnd(short x, short y)
{
    x = LIMIT(x, 0, VRAM_WIDTH - 1);
    y = LIMIT(y, 0, VRAM_HEIGHT - 1);
    return GP0_DRAW_AREA_BR | (y & 0x3FF) << 10 | (x & 0x3FF);
}

static u_long GetOffsetWord(short x, short y)
{
    return GP0_DRAW_OFFSET | (y & 0x7FF) << 11 | (x & 0x7FF);
}

static u_long GetTexWindowWord(RECT *tw)
{
    if (tw) {
        u_long x = (tw->x & 0xFF) >> 3, y = (tw->y & 0xFF) >> 3;
        u_long w = (-tw->w & 0xFF) >> 3, h = (-tw->h & 0xFF) >> 3;
        return GP0_TEX_WINDOW | y << 15 | x << 10 | h << 5 | w;
    }
    return 0;
}

/* The environment's words as SetDrawEnv2 builds them; a background clear fills when 64-pixel aligned. */
static void BuildDrawEnv(DR_ENV *dr_env, DRAWENV *de, int allowFill)
{
    u_long *p = (u_long *)dr_env;
    RECT clip;
    int n = 7;

    p[1] = GetClipStart(de->clip.x, de->clip.y);
    p[2] = GetClipEnd(de->clip.w + de->clip.x - 1, de->clip.y + de->clip.h - 1);
    p[3] = GetOffsetWord(de->ofs[0], de->ofs[1]);
    p[4] = GetModeWord(de->dfe, de->dtd, de->tpage);
    p[5] = GetTexWindowWord(&de->tw);
    p[6] = GP0_MASK_BIT;
    if (de->isbg) {
        clip = de->clip;
        clip.w = LIMIT(clip.w, 0, VRAM_WIDTH - 1);
        clip.h = LIMIT(clip.h, 0, VRAM_HEIGHT - 1);
        if (!allowFill || (clip.x & 0x3F) || (clip.w & 0x3F)) {
            clip.x -= de->ofs[0];
            clip.y -= de->ofs[1];
            p[n++] = GP0_TILE | de->b0 << 16 | de->g0 << 8 | de->r0;
        } else {
            p[n++] = GP0_FILL_RECT | de->b0 << 16 | de->g0 << 8 | de->r0;
        }
        p[n++] = *(u_long *)&clip.x;
        p[n++] = *(u_long *)&clip.w;
    }
    setlen(p, n - 1);
}

void SetDrawEnv(DR_ENV *dr_env, DRAWENV *de)
{
    BuildDrawEnv(dr_env, de, 0);
}

DRAWENV *PutDrawEnv(DRAWENV *de)
{
    BuildDrawEnv(&de->dr_env, de, 1);
    de->dr_env.tag |= OT_END;
    DrawPrim(&de->dr_env);
    sEnv.draw = *de;
    /* The packet's unused words are whatever the caller's record held: not kept. */
    memset(&sEnv.draw.dr_env, 0, sizeof(sEnv.draw.dr_env));
    return de;
}

DRAWENV *GetDrawEnv(DRAWENV *de)
{
    memcpy(de, &sEnv.draw, sizeof(DRAWENV));
    return de;
}

DISPENV *PutDispEnv(DISPENV *de)
{
    g_gpuDisplay.x = de->disp.x;
    g_gpuDisplay.y = de->disp.y;
    g_gpuDisplay.width = de->disp.w;
    g_gpuDisplay.height = de->disp.h;
    g_gpuDisplay.rgb24 = de->isrgb24;
    memcpy(&sEnv.disp, de, sizeof(DISPENV));
    return de;
}

DISPENV *GetDispEnv(DISPENV *de)
{
    memcpy(de, &sEnv.disp, sizeof(DISPENV));
    return de;
}

void SetTexWindow(DR_TWIN *p, RECT *tw)
{
    setlen(p, 2);
    p->code[0] = GetTexWindowWord(tw);
    p->code[1] = 0;
}

void SetDrawArea(DR_AREA *p, RECT *r)
{
    setlen(p, 2);
    p->code[0] = GetClipStart(r->x, r->y);
    p->code[1] = GetClipEnd(r->x + r->w - 1, r->y + r->h - 1);
}

void SetDrawOffset(DR_OFFSET *p, u_short *ofs)
{
    setlen(p, 2);
    p->code[0] = GetOffsetWord(ofs[0], ofs[1]);
    p->code[1] = 0;
}

void SetDrawMode(DR_MODE *p, int dfe, int dtd, int tpage, RECT *tw)
{
    setlen(p, 2);
    p->code[0] = GetModeWord(dfe, dtd, tpage);
    p->code[1] = GetTexWindowWord(tw);
}

int FntPrint(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}
