/* The host: an SDL window and input, and the game in its own context.
 *
 * The game's code is the console's main loop and its waits, unchanged, so it runs in a context of its own
 * (context.c). Every
 * emulated vertical blank (kernel.c) switches back here; the host handles window events, shows the GPU's
 * display area and waits for the real field time before resuming the game. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "context.h"
#include "psxport.h"
#include "gpu.h"
#include "mmio.h"
#include "mdec.h"
#include "spu.h"
#include "kernel_port.h"
#include "netplay.h"
#include "pc_substep.h"
#include "savestate.h"
#include "vk_renderer.h"
#include "settings.h"
#include "overlay.h"
#include "online.h"
#include "input.h"
#include "host.h"

void GameMain(void);

static SDL_Window *sWindow;
static SDL_Renderer *sRenderer;
static SDL_Texture *sTexture;
static int sTextureWidth, sTextureHeight;
static uint32_t sPixels[VRAM_HEIGHT * VRAM_WIDTH];
static const char *sDiscPath = "game_files/Bushido Blade (USA).bin";
static int sQuit;
static Uint64 sNextFieldTime;
static SDL_AudioStream *sAudio;
static unsigned long sField;
static unsigned long sShotEvery;  /* DOJO_SHOT_EVERY: save the display every this many fields */
static unsigned long sExitField;  /* DOJO_EXIT_FIELD: quit at this field */

#define FIELD_NS 16683350ull /* NTSC: 1001/60000 s */

void HostFatal(const char *fmt, ...)
{
    char text[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    fprintf(stderr, "dojo: %s\n", text);
    if (!sExitField) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Dojo", text, sWindow);
    }
    exit(1);
}

/* Called by the translated assembly (tools/mips2c.py) when a computed jump leads neither to its own code
 * nor into the executable: the address came from bad data. */
void AsmBadJump(uint32_t pc, const char *site, const uint32_t *regs)
{
    static const char *const names[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                          "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                          "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
    int i;
    for (i = 0; i < 32; i++) {
        fprintf(stderr, "%s=%08lx%s", names[i], (unsigned long)regs[i], (i % 8 == 7) ? "\n" : " ");
    }
    if (regs[18] > 0x400000 && regs[18] < 0x1000000) {
        const unsigned short *node = (const unsigned short *)(uintptr_t)regs[18];
        fprintf(stderr, "s2 node: renderType %u, words %04x %04x %04x %04x\n", node[0], node[1], node[2], node[3],
                node[4]);
    }
    HostFatal("the renderer jumped to %08lx, which is not code, at %s", (unsigned long)pc, site);
}

const char *HostDiscPath(void)
{
    return sDiscPath;
}

/* The game's folder (game_files/ and saves/ in it): where it was started from, or else the folder of the
 * executable or one of the two above it (the repository root, for port/build/dojo.exe), made current so
 * that started by a double-click or from anywhere it finds the disc and keeps its saves in one place. */
static void FindGameFolder(void)
{
    static const char *const ups[] = {"", "..\\", "..\\..\\"};
    const char *base = SDL_GetBasePath();
    char dir[1024], probe[1200];
    size_t i;
    FILE *f = fopen(sDiscPath, "rb");
    if (f || !base) {
        if (f) {
            fclose(f);
        }
        return;
    }
    for (i = 0; i < sizeof(ups) / sizeof(ups[0]); i++) {
        snprintf(dir, sizeof(dir), "%s%s", base, ups[i]);
        snprintf(probe, sizeof(probe), "%s%s", dir, sDiscPath);
        f = fopen(probe, "rb");
        if (f) {
            fclose(f);
            SetCurrentDirectoryA(dir);
            return;
        }
    }
}

/* DOJO_PRESS: "field:button[+button],..." holds those buttons for 4 fields from each field, for driving
 * the game without a person (start, select, up, down, left, right, x, o, s, t, l1, r1, l2, r2; a "p2"
 * prefix, as in p2x, presses player two's). Returns port `port`'s buttons. */
static unsigned ScriptedButtons(int port)
{
    static const struct {
        const char *name;
        unsigned mask;
    } names[] = {{"start", 1 << 11}, {"select", 1 << 8}, {"up", 1 << 12},  {"right", 1 << 13}, {"down", 1 << 14},
                 {"left", 1 << 15},  {"t", 1 << 4},       {"o", 1 << 5},    {"x", 1 << 6},       {"s", 1 << 7},
                 {"l1", 1 << 2},     {"r1", 1 << 3},      {"l2", 1 << 0},   {"r2", 1 << 1}};
    const char *p = SDL_getenv("DOJO_PRESS");
    unsigned buttons = 0;
    while (p && *p) {
        char *end;
        unsigned long at = strtoul(p, &end, 10);
        p = end;
        if (*p++ != ':') {
            break;
        }
        while (*p && *p != ',') {
            size_t i, len = strcspn(p, "+,");
            const char *name = p;
            int namePort = 0;
            if (len > 2 && strncmp(name, "p2", 2) == 0) {
                name += 2;
                namePort = 1;
            }
            for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
                if (namePort == port && strlen(names[i].name) == len - (size_t)(name - p) &&
                    strncmp(name, names[i].name, len - (name - p)) == 0 && sField >= at && sField < at + 4) {
                    buttons |= names[i].mask;
                }
            }
            p += len;
            if (*p == '+') {
                p++;
            }
        }
        if (*p == ',') {
            p++;
        }
    }
    return buttons;
}

/* This machine's first player (input.c) and the script's player one; online, where this machine plays one
 * fighter, either player's controls work it. */
unsigned HostLocalButtons(void)
{
    unsigned buttons;
    if (OverlayIsOpen()) {
        return ScriptedButtons(0);
    }
    buttons = InputButtons(0) | (NetplayActive() ? InputButtons(1) : 0) | ScriptedButtons(0);
    if (SDL_getenv("DOJO_PRESS_TICKS")) {
        /* For testing: a controller that changes from one sample to the next (left and right taps, square,
         * triangle, circle), as a person's can between two ticks, on a pattern the seed varies. */
        static unsigned n;
        static const unsigned pattern[8] = {1u << 15, 0, 1u << 13, 1u << 7, 0, 1u << 4, 1u << 15 | 1u << 5, 0};
        unsigned seed = (unsigned)atoi(SDL_getenv("DOJO_PRESS_TICKS"));
        n++;
        buttons |= pattern[((n * 2654435761u + seed * 40503u) >> 7) % 8];
    }
    return buttons;
}

void HostFillPads(void)
{
    HostWritePadPort(0, 1, HostLocalButtons(), NULL);
    /* The second controller is always plugged in, as on a console with two pads: VS mode needs it. */
    HostWritePadPort(1, 1, (OverlayIsOpen() ? 0 : InputButtons(1)) | ScriptedButtons(1), NULL);
}

/* DOJO_WAV: records everything the SPU produces to a WAV file, for checking sound without listening. */
static void RecordAudio(const short *stereo, int frames)
{
    static FILE *wav;
    static uint32_t bytes;
    static int checked;
    uint32_t riff;
    if (!checked) {
        const char *path = SDL_getenv("DOJO_WAV");
        static const unsigned char header[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't',
                                                 ' ', 16, 0, 0, 0, 1, 0, 2, 0, 0x44, 0xAC, 0, 0, 0x10, 0xB1, 2,
                                                 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
        checked = 1;
        wav = path ? fopen(path, "wb") : NULL;
        if (wav) {
            fwrite(header, 1, sizeof(header), wav);
        }
    }
    if (!wav) {
        return;
    }
    fwrite(stereo, 4, frames, wav);
    bytes += frames * 4;
    riff = bytes + 36;
    fseek(wav, 4, SEEK_SET);
    fwrite(&riff, 4, 1, wav);
    fseek(wav, 40, SEEK_SET);
    fwrite(&bytes, 4, 1, wav);
    fseek(wav, 0, SEEK_END);
    fflush(wav);
}

void HostQueueAudio(const short *stereo, int frames)
{
    {
        /* the field's sound effects, for a DualSense's haptics (a field run again after a rollback has neither) */
        int hapticFrames;
        const int16_t *haptic = SpuTakeFieldHaptics(&hapticFrames);
        InputQueueHaptics(haptic, hapticFrames);
    }
    RecordAudio(stereo, frames);
    /* Keep at most a fifth of a second queued: running ahead (fast-forward, a stall) drops sound, not time. */
    if (!sAudio || SDL_GetAudioStreamQueued(sAudio) > SPU_SAMPLE_RATE * 4 / 5) {
        return;
    }
    SDL_PutAudioStreamData(sAudio, stereo, frames * 4);
}

/* The motor bytes are the game's pulse turned on and off; input.c reads the pulse's strength instead. */
void HostSetRumbleBuffers(unsigned char *bufA, long lenA, unsigned char *bufB, long lenB)
{
    (void)bufA;
    (void)lenA;
    (void)bufB;
    (void)lenB;
}

static void SaveShot(const uint32_t *pixels, int w, int h);
static int sVulkan;

/* Widescreen (psxport.h). */
int g_pcScreenMargin;
int g_pcWideHeadings;

/* DOJO_ASPECT, as "16:9" (the default), "21:9", "4:3" or a ratio: the display buffers are made wider
 * by the margin that gives that shape at the console's pixel shape, 640x240 being 4:3. The stage cull adds
 * a heading (5.625 degrees) to each side for every 5.625 degrees the half field of view grows. */
/* The widescreen setting as the simulation sees it (psxport.h). */
static void SetSimulationMargin(int margin)
{
    /* The half field of view at the console's projection distance (772), and its growth in headings. */
    double extra = (SDL_atan((320.0 + margin) / 772.0) - SDL_atan(320.0 / 772.0)) * 180.0 / SDL_PI_D;
    g_pcScreenMargin = margin;
    g_pcWideHeadings = (int)SDL_ceil(extra / 5.625);
}

static int sRunningAspect; /* the SETTINGS_ASPECTS index the session runs with, -1 for another */
static int sOwnMargin;     /* this machine's widescreen margin (an online match runs another) */
static int sDisplayW, sDisplayH; /* the display "auto" took its shape from */

static void SetAspect(void)
{
    const char *text = SDL_getenv("DOJO_ASPECT");
    double w = 16, h = 9, aspect;
    int i;
    if (!text) {
        text = SETTINGS_ASPECTS[g_settings.aspect];
    }
    sRunningAspect = -1;
    for (i = 0; i < SETTINGS_ASPECT_COUNT; i++) {
        if (SDL_strcasecmp(text, SETTINGS_ASPECTS[i]) == 0) {
            sRunningAspect = i;
        }
    }
    if (sRunningAspect == SETTINGS_ASPECT_AUTO) {
        /* The shape of the main display's own resolution. */
        const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        if (mode && mode->w > 0 && mode->h > 0) {
            w = mode->w;
            h = mode->h;
        }
        sDisplayW = mode ? mode->w : 0;
        sDisplayH = mode ? mode->h : 0;
    } else if (sscanf(text, "%lf:%lf", &w, &h) != 2 && sscanf(text, "%lf", &w) == 1) {
        h = 1;
    }
    aspect = (w > 0 && h > 0) ? w / h : 4.0 / 3.0;
    if (aspect < 4.0 / 3.0) {
        aspect = 4.0 / 3.0;
    }
    g_pcScreenMargin = (int)((480.0 * aspect - 640.0) / 2.0 + 0.5);
    if (g_pcScreenMargin > 380) {
        g_pcScreenMargin = 380; /* the GTE clamps screen x at 1023 */
    }
    SetSimulationMargin(g_pcScreenMargin);
    sOwnMargin = g_pcScreenMargin;
}

static SDL_Texture *sOverlayTexture;
static void UpdateOverlayContext(void);

static int DisplayHz(void);
static int sPresentForced = 1; /* the window changed: present even an unchanged picture */

/* Whether this picture is shown: not when nothing changed since the last one shown (a field's end inside a
 * tick, a menu's second field), and with vsync, not before the display's next refresh is due (the
 * simulation's rate, a multiple of 30, isn't the display's; a driver set to ignore vsync would otherwise show
 * every one). */
static int PresentDue(int overlayChanged)
{
    static unsigned shownDraws;
    static GpuDisplay shownDisplay;
    static Uint64 nextNs;
    Uint64 now = SDL_GetTicksNS(), interval;
    if (!sPresentForced && !overlayChanged && g_gpuDrawCount == shownDraws &&
        memcmp(&g_gpuDisplay, &shownDisplay, sizeof(shownDisplay)) == 0) {
        return 0;
    }
    if (g_settings.vsync && !sPresentForced) {
        interval = 1000000000ull / (Uint64)DisplayHz();
        if (now + interval / 8 < nextNs) {
            return 0; /* the display can't show it before the next one is ready */
        }
        nextNs = now > nextNs + interval ? now + interval : nextNs + interval;
    }
    sPresentForced = 0;
    shownDraws = g_gpuDrawCount;
    shownDisplay = g_gpuDisplay;
    return 1;
}

/* Converts the display area of video memory to the texture and draws it 4:3 in the window. */
static void Present(void)
{
    const GpuDisplay *d = &g_gpuDisplay;
    int w = d->width > 0 ? d->width : 640;
    int h = d->height > 0 ? d->height : 240;
    int x, y;
    SDL_FRect dst;
    int winW, winH;

    int overlayChanged;
    const uint32_t *overlay;

    UpdateOverlayContext();
    overlay = OverlayPicture(&overlayChanged);
    if (sShotEvery && sField % sShotEvery == 0) {
        sPresentForced = 1; /* a test's screenshot is taken from the presented picture */
    }
    if (!PresentDue(overlayChanged)) {
        return;
    }
    OverlayFramePresented();
    if (overlay && overlayChanged && SDL_getenv("DOJO_MENU_AT")) {
        /* A test's look at the overlay: over mid grey, as menu.bmp. */
        static uint32_t flat[OVERLAY_WIDTH * OVERLAY_HEIGHT];
        unsigned long savedField = sField;
        int i;
        for (i = 0; i < OVERLAY_WIDTH * OVERLAY_HEIGHT; i++) {
            uint32_t p = overlay[i], a = p >> 24, out = 0, c;
            for (c = 0; c < 3; c++) {
                uint32_t v = (p >> (c * 8)) & 0xFF;
                out |= ((v * a + 96 * (255 - a)) / 255) << ((2 - c) * 8);
            }
            flat[i] = out;
        }
        sField = 999999;
        SaveShot(flat, OVERLAY_WIDTH, OVERLAY_HEIGHT);
        sField = savedField;
    }
    if (sVulkan) {
        if (sShotEvery && sField % sShotEvery == 0) {
            int cw, ch;
            const uint32_t *pixels = VkRendererCapture(&cw, &ch);
            SaveShot(pixels, cw, ch);
        }
        VkRendererSetOverlay(overlay, overlayChanged);
        VkRendererPresent(4.0f / 3.0f);
        return;
    }

    if (w > VRAM_WIDTH) w = VRAM_WIDTH;
    if (h > VRAM_HEIGHT) h = VRAM_HEIGHT;
    if (w != sTextureWidth || h != sTextureHeight) {
        if (sTexture) {
            SDL_DestroyTexture(sTexture);
        }
        sTexture = SDL_CreateTexture(sRenderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        SDL_SetTextureScaleMode(sTexture, SDL_SCALEMODE_NEAREST);
        sTextureWidth = w;
        sTextureHeight = h;
    }
    for (y = 0; y < h; y++) {
        uint32_t *out = &sPixels[y * w];
        int vy = (d->y + y) & (VRAM_HEIGHT - 1);
        if (!d->enabled) {
            memset(out, 0, w * 4);
            continue;
        }
        if (d->rgb24) {
            const uint8_t *row = (const uint8_t *)g_vram[vy];
            for (x = 0; x < w; x++) {
                int byte = (d->x * 2 + x * 3) & (VRAM_WIDTH * 2 - 1);
                out[x] = (uint32_t)row[byte] << 16 | (uint32_t)row[(byte + 1) & 2047] << 8 | row[(byte + 2) & 2047];
            }
        } else {
            for (x = 0; x < w; x++) {
                uint16_t c = g_vram[vy][(d->x + x) & (VRAM_WIDTH - 1)];
                uint32_t r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3;
                out[x] = (r | r >> 5) << 16 | (g | g >> 5) << 8 | (b | b >> 5);
            }
        }
    }
    SDL_UpdateTexture(sTexture, NULL, sPixels, w * 4);
    if (sShotEvery && sField % sShotEvery == 0) {
        SaveShot(sPixels, w, h);
    }
    SDL_GetRenderOutputSize(sRenderer, &winW, &winH);
    dst.h = (float)winH;
    dst.w = dst.h * 4.0f / 3.0f;
    if (dst.w > winW) {
        dst.w = (float)winW;
        dst.h = dst.w * 3.0f / 4.0f;
    }
    dst.x = (winW - dst.w) / 2;
    dst.y = (winH - dst.h) / 2;
    SDL_SetRenderDrawColor(sRenderer, 0, 0, 0, 255);
    SDL_RenderClear(sRenderer);
    SDL_RenderTexture(sRenderer, sTexture, NULL, &dst);
    if (overlay) {
        /* The options overlay at 16:9 over the whole window. */
        if (!sOverlayTexture) {
            sOverlayTexture = SDL_CreateTexture(sRenderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                                                OVERLAY_WIDTH, OVERLAY_HEIGHT);
            SDL_SetTextureBlendMode(sOverlayTexture, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(sOverlayTexture, SDL_SCALEMODE_NEAREST);
            overlayChanged = 1;
        }
        if (overlayChanged) {
            SDL_UpdateTexture(sOverlayTexture, NULL, overlay, OVERLAY_WIDTH * 4);
        }
        dst.w = (float)winW;
        dst.h = dst.w * OVERLAY_HEIGHT / OVERLAY_WIDTH;
        if (dst.h > winH) {
            dst.h = (float)winH;
            dst.w = dst.h * OVERLAY_WIDTH / OVERLAY_HEIGHT;
        }
        dst.x = (winW - dst.w) / 2;
        dst.y = (winH - dst.h) / 2;
        SDL_RenderTexture(sRenderer, sOverlayTexture, NULL, &dst);
    }
    SDL_RenderPresent(sRenderer);
}

/* Saves the shown picture as a 24-bit BMP (shot_<field>.bmp), for checking the port without watching. */
static void SaveShot(const uint32_t *pixels, int w, int h)
{
    char name[64];
    FILE *f;
    int y, x;
    uint32_t rowBytes = (uint32_t)((w * 3 + 3) & ~3);
    uint32_t size = 54 + rowBytes * h;
    unsigned char header[54] = {'B', 'M'};
    snprintf(name, sizeof(name), "shot_%06lu.bmp", sField);
    f = fopen(name, "wb");
    if (!f) {
        return;
    }
    memcpy(&header[2], &size, 4);
    header[10] = 54;
    header[14] = 40;
    memcpy(&header[18], &w, 4);
    memcpy(&header[22], &h, 4);
    header[26] = 1;
    header[28] = 24;
    fwrite(header, 1, 54, f);
    for (y = h - 1; y >= 0; y--) {
        static unsigned char row[8192 * 3 + 4];
        memset(row, 0, rowBytes);
        for (x = 0; x < w && x < 8192; x++) {
            uint32_t p = pixels[y * w + x];
            row[x * 3] = (unsigned char)p;
            row[x * 3 + 1] = (unsigned char)(p >> 8);
            row[x * 3 + 2] = (unsigned char)(p >> 16);
        }
        fwrite(row, 1, rowBytes, f);
    }
    fclose(f);
}

static void PumpEvents(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (OverlayHandleEvent(&e)) {
            continue;
        }
        switch (e.type) {
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_RESTORED:
            sPresentForced = 1;
            break;
        case SDL_EVENT_QUIT:
            sQuit = 1;
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
            InputDevicesChanged();
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.scancode == SDL_SCANCODE_F11) {
                g_settings.fullscreen = !(SDL_GetWindowFlags(sWindow) & SDL_WINDOW_FULLSCREEN);
                SDL_SetWindowFullscreen(sWindow, g_settings.fullscreen);
                SettingsSave();
            }
            break;
        default:
            break;
        }
    }
}

void HostVerticalBlank(void)
{
    ContextSwitchToHost();
}

/* On a crash, prints the faulting address and the frame-pointer chain; resolve them with
 * llvm-symbolizer --obj=dojo.exe <addresses>. A vectored handler, so it runs on the game's stack too,
 * whose handler chain is empty; it lets every exception that is not a fault go on to the usual handling. */
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS *info)
{
    uintptr_t *frame = (uintptr_t *)info->ContextRecord->Ebp;
    int i;
    switch (info->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    fprintf(stderr,"crash: exception %08lx at %08lx (field %lu)\n", info->ExceptionRecord->ExceptionCode,
            (unsigned long)info->ContextRecord->Eip, sField);
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        fprintf(stderr, "crash: %s %08lx\n", info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                (unsigned long)info->ExceptionRecord->ExceptionInformation[1]);
    }
    {
        uint32_t *sp = (uint32_t *)info->ContextRecord->Esp;
        fprintf(stderr, "regs: esp %08lx ebp %08lx eax %08lx ecx %08lx edx %08lx\n",
                (unsigned long)info->ContextRecord->Esp, (unsigned long)info->ContextRecord->Ebp,
                (unsigned long)info->ContextRecord->Eax, (unsigned long)info->ContextRecord->Ecx,
                (unsigned long)info->ContextRecord->Edx);
        fprintf(stderr, "esp:");
        for (i = 0; i < 16 && !IsBadReadPtr(sp + i, 4); i++) {
            fprintf(stderr, " %08lx", (unsigned long)sp[i]);
        }
        fprintf(stderr, "\n");
    }
    {
        void TracePrint(FILE *out);
        TracePrint(stderr);
    }
    fprintf(stderr, "stack: %08lx", (unsigned long)info->ContextRecord->Eip);
    for (i = 0; i < 32 && frame && !IsBadReadPtr(frame, 8); i++) {
        fprintf(stderr, " %08lx", (unsigned long)frame[1]);
        frame = (uintptr_t *)frame[0];
    }
    fprintf(stderr, "\n");
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* In scripted runs, a thread that reports where the game is when no field has passed for three seconds:
 * a wait on emulated hardware that never ends. Resolve the address with llvm-symbolizer. */
static HANDLE sMainThread;

static DWORD WINAPI Watchdog(void *param)
{
    unsigned long lastField = 0;
    int stalled = 0;
    (void)param;
    for (;;) {
        Sleep(1000);
        if (sField != lastField || OverlayIsOpen() || NetplayActive() || OnlineWantsToWatch()) {
            /* the game moved on, or waits for the menu on purpose */
            lastField = sField;
            stalled = 0;
            continue;
        }
        if (++stalled == 3) {
            CONTEXT ctx;
            ctx.ContextFlags = CONTEXT_CONTROL;
            SuspendThread(sMainThread);
            GetThreadContext(sMainThread, &ctx);
            fprintf(stderr, "stalled: field %lu, eip %08lx, ebp %08lx\n", sField, (unsigned long)ctx.Eip,
                    (unsigned long)ctx.Ebp);
            {
                uintptr_t *frame = (uintptr_t *)ctx.Ebp;
                int i;
                fprintf(stderr, "stack: %08lx", (unsigned long)ctx.Eip);
                for (i = 0; i < 16 && frame && !IsBadReadPtr(frame, 8); i++) {
                    fprintf(stderr, " %08lx", (unsigned long)frame[1]);
                    frame = (uintptr_t *)frame[0];
                }
                fprintf(stderr, "\n");
            }
            fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 4);
        }
    }
}

/* DOJO_STATE_TEST="F,G": saves the simulation at field F, runs to G and notes its hash, restores F, runs
 * to G again and compares: a save state that misses any of the simulation shows as two hashes. */
static void StateRoundTripTest(void)
{
    static int parsed, done;
    static unsigned long saveField, checkField;
    static uint8_t *state;
    static uint64_t firstHash;
    const char *spec;

    if (!parsed) {
        parsed = 1;
        spec = SDL_getenv("DOJO_STATE_TEST");
        if (!spec || sscanf(spec, "%lu,%lu", &saveField, &checkField) != 2 || checkField <= saveField) {
            done = 1;
        }
    }
    if (done) {
        return;
    }
    if (sField == saveField && !state) {
        state = SDL_malloc(SaveStateCapacity());
        fprintf(stderr, "state test: saved %u bytes at field %lu\n", (unsigned)SaveStateSave(state), sField);
    } else if (sField == checkField && firstHash == 0) {
        firstHash = SaveStateHash();
        SaveStateLoad(state);
        sField = saveField;
        fprintf(stderr, "state test: hash %016llx at field %lu; restored field %lu\n",
                (unsigned long long)firstHash, checkField, saveField);
    } else if (sField == checkField) {
        uint64_t again = SaveStateHash();
        fprintf(stderr, "state test: hash %016llx again: %s\n", (unsigned long long)again,
                again == firstHash ? "identical" : "DIFFERENT");
        done = 1;
    }
}

/* Waits until real time reaches the emulated time the game has run to: a field is 263 lines of 1001/60000 s. */
static Uint64 sPaceStart;

static void PaceToEmulatedTime(void)
{
    Uint64 target = sPaceStart + (Uint64)KernelLine() * FIELD_NS / 263;
    Uint64 now = SDL_GetTicksNS();
    if (SDL_getenv("DOJO_FAST")) {
        return;
    }
    if (now < target) {
        SDL_DelayPrecise(target - now);
    } else if (now - target > 100000000ull) {
        /* Fell far behind (a breakpoint, a long load): carry on from now rather than race to catch up. */
        sPaceStart += now - target;
    }
}

/* A tick inside a netplay field: shown, and paced. */
void HostPresentTick(void)
{
    PumpEvents();
    Present();
    PaceToEmulatedTime();
}

static int DisplayHz(void)
{
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(sWindow));
    if (SDL_getenv("DOJO_DISPLAY_HZ")) {
        return SDL_atoi(SDL_getenv("DOJO_DISPLAY_HZ")); /* for a test: as if the display ran at this rate */
    }
    return (mode && mode->refresh_rate > 0) ? (int)(mode->refresh_rate + 0.5f) : 60;
}

/* The frame-rate setting (DOJO_FPS sets it for the session), or the display's refresh rate: ticks per
 * console frame, a whole number of 30ths. */
static int ChooseTicksPerFrame(void)
{
    int fps = g_settings.fps > 0 ? g_settings.fps : DisplayHz(), ticks;
    ticks = (fps + 15) / 30;
    return ticks < 1 ? 1 : (ticks > 16 ? 16 : ticks);
}

static void UpdateOverlayContext(void)
{
    OverlayContext context;
    context.vulkan = sVulkan;
    context.online = NetplayActive();
    context.runningAspect = sRunningAspect;
    context.displayW = sDisplayW;
    context.displayH = sDisplayH;
    context.runningSoftware = !sVulkan;
    context.displayHz = DisplayHz();
    context.ticksPerFrame = g_pcTicksPerFrame;
    OverlayUpdateContext(&context);
}

/* A setting changed in the options overlay. */
static void ApplySetting(OverlayChange change)
{
    switch (change) {
    case OVERLAY_CHANGE_FPS:
        /* Offline only (an online match keeps its agreed rate): from the next console frame (src/main.c). */
        if (!NetplayActive()) {
            g_pcTicksPerFrame = ChooseTicksPerFrame();
        }
        break;
    case OVERLAY_CHANGE_SCALE:
        if (sVulkan) {
            VkRendererSetScale(g_settings.scale);
        }
        break;
    case OVERLAY_CHANGE_FULLSCREEN:
        SDL_SetWindowFullscreen(sWindow, g_settings.fullscreen);
        break;
    case OVERLAY_CHANGE_VSYNC:
        if (sVulkan) {
            VkRendererSetVsync(g_settings.vsync);
        } else {
            SDL_SetRenderVSync(sRenderer, g_settings.vsync ? 1 : 0);
        }
        break;
    case OVERLAY_CHANGE_VOLUME:
        if (sAudio) {
            SDL_SetAudioStreamGain(sAudio, g_settings.volume / 100.0f);
        }
        break;
    case OVERLAY_CHANGE_QUIT:
        sQuit = 1;
        break;
    }
}

static unsigned char *sBootState;

const unsigned char *HostBootState(void)
{
    return sBootState;
}

void HostResetPacing(void)
{
    sPaceStart = SDL_GetTicksNS() - (Uint64)KernelLine() * FIELD_NS / 263;
}

void HostSetSimulationMargin(int margin)
{
    SetSimulationMargin(margin);
}

int HostOwnMargin(void)
{
    return sOwnMargin;
}

int HostOwnTicksPerFrame(void)
{
    return ChooseTicksPerFrame();
}

static void GameEntry(void)
{
    GameMain();
    sQuit = 1;
}

int main(int argc, char **argv)
{
    AddVectoredExceptionHandler(1, CrashHandler);
    if (!MemoryInit()) {
        HostFatal("could not map the console's RAM at 0x80000000 (the port needs 64-bit Windows)");
    }
    if (!MmioInit()) {
        HostFatal("could not map the console's I/O registers at 0x1F800000");
    }
    MdecCheckTables();
    setvbuf(stdout, NULL, _IONBF, 0); /* the game's own printf output, kept up to a crash */
    if (argc > 1 && strncmp(argv[1], "--", 2) == 0) {
        /* The launcher's helpers for its Controls tab (input.c): no window, no game. */
        int code;
        setvbuf(stdout, NULL, _IONBF, 0);
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
            return 1;
        }
        SettingsLoad(0);
        InputInit();
        code = InputTool(argv[1], argc > 2 ? argv[2] : NULL);
        InputShutdown();
        SDL_Quit();
        return code < 0 ? 2 : code;
    }
    if (argc > 1) {
        sDiscPath = argv[1];
    } else {
        FindGameFolder();
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        HostFatal("SDL_Init: %s", SDL_GetError());
    }
    /* dojo.ini (a scripted run keeps the defaults), then the environment for this session. */
    SettingsLoad(SDL_getenv("DOJO_EXIT_FIELD") != NULL || SDL_getenv("DOJO_HIDDEN") != NULL);
    {
        const char *fps = SDL_getenv("DOJO_FPS");
        const char *scale = SDL_getenv("DOJO_SCALE");
        const char *which = SDL_getenv("DOJO_RENDERER");
        const char *quickPing = SDL_getenv("DOJO_QUICK_MAX_PING");
        if (fps) {
            g_settings.fps = atoi(fps) > 0 ? atoi(fps) : 30;
        }
        if (quickPing) {
            g_settings.quickMaxPing = atoi(quickPing);
        }
        if (SDL_getenv("DOJO_FIRST_TO")) {
            g_settings.firstTo = atoi(SDL_getenv("DOJO_FIRST_TO"));
        }
        if (scale) {
            g_settings.scale = atoi(scale) < 1 ? 1 : (atoi(scale) > 8 ? 8 : atoi(scale));
        }
        if (which) {
            g_settings.software = strcmp(which, "software") == 0;
        }
    }
    SetAspect();
    if (SDL_getenv("DOJO_HIDDEN")) {
        /* For tests run while someone uses the machine: no window on the screen, drawn on the CPU (the
         * simulation is the same with either renderer). */
        g_settings.software = 1;
    }
    {
        /* Vulkan draws at the scale setting (default 4) times the console's resolution; the software
         * setting, or no Vulkan, draws at 1x on the CPU. */
        if (!g_settings.software) {
            sWindow = SDL_CreateWindow("Dojo", 1600, 900, SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
            if (sWindow && VkRendererInit(sWindow, g_settings.scale, g_pcScreenMargin)) {
                sVulkan = 1;
            } else if (sWindow) {
                SDL_DestroyWindow(sWindow);
                sWindow = NULL;
            }
        }
    }
    if (!sVulkan) {
        sWindow = SDL_CreateWindow("Dojo", 1280, 960,
                                   SDL_WINDOW_RESIZABLE | (SDL_getenv("DOJO_HIDDEN") ? SDL_WINDOW_HIDDEN : 0));
        if (!sWindow) {
            HostFatal("SDL_CreateWindow: %s", SDL_GetError());
        }
        sRenderer = SDL_CreateRenderer(sWindow, NULL);
        if (!sRenderer) {
            HostFatal("SDL_CreateRenderer: %s", SDL_GetError());
        }
        SDL_SetRenderVSync(sRenderer, g_settings.vsync ? 1 : 0);
    }
    if (sVulkan && g_settings.vsync) {
        VkRendererSetVsync(1);
    }
    SDL_SetWindowTitle(sWindow, "Dojo   (F1: options)");
    if (g_settings.fullscreen) {
        SDL_SetWindowFullscreen(sWindow, true);
    }
    {
        SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, SPU_SAMPLE_RATE};
        sAudio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (sAudio) {
            SDL_SetAudioStreamGain(sAudio, g_settings.volume / 100.0f);
            SDL_ResumeAudioStreamDevice(sAudio);
        }
    }
    InputInit();

    g_pcTicksPerFrame = ChooseTicksPerFrame();
    {
        int agreedMargin, agreedTicks;
        if (NetplayStart(g_pcScreenMargin, g_pcTicksPerFrame, &agreedMargin, &agreedTicks)) {
            /* The display keeps its own margin and refresh; the simulation takes the wider and the faster. */
            SetSimulationMargin(agreedMargin);
            g_pcTicksPerFrame = agreedTicks;
        }
    }
    fprintf(stderr, "simulation: %d ticks a frame (%d a second)\n", g_pcTicksPerFrame, g_pcTicksPerFrame * 30);
    {
        OverlayContext context;
        memset(&context, 0, sizeof(context));
        OverlayInit(&context, ApplySetting);
        UpdateOverlayContext();
    }
    ContextInit(GameEntry);
    /* Power-on, kept for online play: every match starts from it (online.c). */
    sBootState = SDL_malloc(SaveStateCapacity());
    SaveStateSave(sBootState);

    {
        const char *every = SDL_getenv("DOJO_SHOT_EVERY");
        const char *exitAt = SDL_getenv("DOJO_EXIT_FIELD");
        sShotEvery = every ? strtoul(every, NULL, 10) : 0;
        sExitField = exitAt ? strtoul(exitAt, NULL, 10) : 0;
    }
    if (sExitField) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &sMainThread, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        CreateThread(NULL, 0, Watchdog, NULL, 0, NULL);
        fprintf(stderr, "memory: psx ram %p, scratchpad %p, vram %p\n", (void *)g_psxRam, (void *)g_psxScratch,
                (void *)g_vram);
    }

    sPaceStart = SDL_GetTicksNS();
    while (!sQuit) {
        int newField;
        PumpEvents();
        {
            /* DOJO_MENU_AT=field: opens the options overlay there, for looking at it in a test. */
            static int opened;
            const char *at = SDL_getenv("DOJO_MENU_AT");
            if (at && !opened && KernelFieldCount() >= strtoul(at, NULL, 10)) {
                /* DOJO_MENU_KEYS: keys pressed in it then, by SDL's names, as "Down,Down,Return". */
                const char *keys = SDL_getenv("DOJO_MENU_KEYS");
                opened = 1;
                OverlayOpen();
                while (keys && *keys) {
                    char name[32];
                    size_t len = strcspn(keys, ",");
                    SDL_Event e;
                    snprintf(name, sizeof(name), "%.*s", (int)len, keys);
                    memset(&e, 0, sizeof(e));
                    e.key.scancode = SDL_GetScancodeFromName(name);
                    e.type = SDL_EVENT_KEY_DOWN;
                    OverlayHandleEvent(&e);
                    e.type = SDL_EVENT_KEY_UP;
                    OverlayHandleEvent(&e);
                    keys += len + (keys[len] == ',');
                }
            }
        }
        {
            /* DOJO_APPLY_AT="field:scale=N,field:vsync=N,field:fps=N": settings changed as the menu would,
             * for testing them in a scripted run. */
            static unsigned long lastField = (unsigned long)-1;
            const char *p = SDL_getenv("DOJO_APPLY_AT");
            unsigned long now = KernelFieldCount();
            while (p && *p && now != lastField) {
                char name[16];
                unsigned long at;
                int value, n = 0;
                if (sscanf(p, "%lu:%15[a-z]=%d%n", &at, name, &value, &n) != 3) {
                    break;
                }
                if (at == now) {
                    if (strcmp(name, "scale") == 0) {
                        g_settings.scale = value;
                        ApplySetting(OVERLAY_CHANGE_SCALE);
                    } else if (strcmp(name, "vsync") == 0) {
                        g_settings.vsync = value;
                        ApplySetting(OVERLAY_CHANGE_VSYNC);
                    } else if (strcmp(name, "fps") == 0) {
                        g_settings.fps = value;
                        ApplySetting(OVERLAY_CHANGE_FPS);
                    }
                    fprintf(stderr, "field %lu: %s = %d\n", now, name, value);
                }
                p += n;
                p += *p == ',';
            }
            lastField = now;
        }
        {
            /* DOJO_ONLINE: quick, create, join:CODE, koth or koth:CODE: goes online and asks for that, for
             * testing online play from a script. */
            static int asked;
            const char *action = SDL_getenv("DOJO_ONLINE");
            if (!asked && SDL_getenv("DOJO_REPLAY")) {
                /* DOJO_REPLAY=file: plays that replay (in replays/), for testing */
                asked = 1;
                OnlinePlayReplay(SDL_getenv("DOJO_REPLAY"));
            }
            if (action && !asked && strcmp(action, "host") == 0) {
                asked = 1;
                OnlineHostDirect();
            } else if (action && !asked && strncmp(action, "connect:", 8) == 0) {
                asked = 1;
                OnlineJoinDirect(action + 8);
            } else if (action && !asked) {
                if (OnlineGetState() == ONLINE_OFF) {
                    OnlineConnect();
                } else if (OnlineGetState() == ONLINE_IDLE) {
                    asked = 1;
                    if (strcmp(action, "quick") == 0) {
                        OnlineQuickPlay();
                    } else if (strcmp(action, "create") == 0) {
                        OnlineCreateRoom();
                    } else if (strncmp(action, "join:", 5) == 0) {
                        OnlineJoinRoom(action + 5);
                    } else if (strncmp(action, "koth", 4) == 0) {
                        OnlineKingOfTheHill(action[4] == ':' ? action + 5 : "");
                    }
                }
            }
        }
        if (OnlineUpdate()) {
            /* Online play is getting a match's starting state ready, with the game itself. */
            Present();
            continue;
        }
        if (OverlayIsOpen() && !NetplayActive() && !OnlineWantsToWatch()) {
            /* The menu is open: the game waits, its last picture shown under it. */
            InputUpdateRumble(0);
            Present();
            SDL_Delay(8);
            continue;
        }
        if (NetplayActive()) {
            /* Online, the session runs the field: both controllers, rollbacks, waiting for the other. */
            newField = NetplayRunField(HostLocalButtons());
            if (newField && KernelFieldCount() % 30 == 0) {
                SDL_SetWindowTitle(sWindow, NetplayStatus());
            }
        } else if (OnlineWantsToWatch()) {
            /* Waiting on the hill: the fight being fought there, from its log (online.c). */
            newField = OnlineWatchField();
        } else {
            /* The game runs to its next yield: a vertical blank, or the end of a tick. The controllers are
             * written before each tick, not only at vertical blanks as the console's BIOS does, so at a high
             * rate a press counts from the next tick; the field's sound is taken after a blank: host work,
             * done here on the host's stack rather than the game's. */
            HostFillPads();
            ContextSwitchToGame();
            newField = KernelLastYieldWasBlank();
            if (newField) {
                int frames;
                const int16_t *samples = SpuTakeFieldAudio(&frames);
                if (frames) {
                    HostQueueAudio(samples, frames);
                }
            }
        }
        sField = KernelFieldCount();
        if (!newField) {
            Present();
            if (NetplayActive() || OnlineWantsToWatch()) {
                SDL_Delay(2); /* waiting for the other machine, or the fight's log: time does not move */
            } else {
                PaceToEmulatedTime(); /* a tick inside the field */
            }
            continue;
        }
        InputUpdateRumble(!OnlineWantsToWatch());
        StateRoundTripTest();
        {
            /* DOJO_HASH_EVERY: logs the simulation's hash every this many fields, to compare runs. */
            static unsigned long every = (unsigned long)-1;
            if (every == (unsigned long)-1) {
                const char *text = SDL_getenv("DOJO_HASH_EVERY");
                every = text ? strtoul(text, NULL, 10) : 0;
            }
            if (every && sField % every == 0) {
                fprintf(stderr, "hash %lu %016llx\n", sField, (unsigned long long)SaveStateHash());
            }
            {
                /* DOJO_DUMP_STATE=field: writes the save state there to state_<field>.bin, with the
                 * addresses of its regions, for finding what two runs disagree on. */
                const char *text = SDL_getenv("DOJO_DUMP_STATE");
                if (text && strtoul(text, NULL, 10) == sField) {
                    uint8_t *state = SDL_malloc(SaveStateCapacity());
                    size_t size = SaveStateSave(state);
                    char name[64];
                    FILE *f;
                    snprintf(name, sizeof(name), "state_%06lu.bin", sField);
                    f = fopen(name, "wb");
                    if (f) {
                        fwrite(state, 1, size, f);
                        fclose(f);
                    }
                    SDL_free(state);
                    SaveStatePrintRegions(stderr);
                }
            }
        }
        if (sExitField) {
            /* A scripted run logs the game's state changes. */
            extern short gGameState;
            static short lastState = -1;
            if (gGameState != lastState) {
                fprintf(stderr, "field %lu: game state %d\n", sField, gGameState);
                lastState = gGameState;
            }
        }
        Present();
        if (sExitField && sField >= sExitField) {
            if (NetplayActive()) {
                fprintf(stderr, "netplay: %s\n", NetplayStatus());
            }
            break;
        }
        PaceToEmulatedTime();
    }
    if (OverlayIsOpen()) {
        SettingsSave(); /* the window closed with the menu up: what was changed in it (shutting it saves too) */
    }
    OnlineDisconnect();
    InputShutdown();
    if (sVulkan) {
        VkRendererShutdown();
    }
    SDL_Quit();
    return 0;
}
