/* The console's kernel services, interrupts and video timing for the port.
 *
 * Time is emulated, not measured: the horizontal-blank count advances one line each time the game reads it
 * and jumps to the next vertical blank when the game waits for one. With substeps (pc_substep.h) a frame's
 * ticks end at their shares of its two fields instead, and the count is of reads alone, so the frame-pacing
 * spin calls rand() as often as with one tick and the random sequence does not depend on the tick rate. The main loop spins rand() until 384
 * lines have passed (src/main.c), so every frame advances the random sequence the same number of times and
 * lasts two fields, as an unloaded console frame does: the simulation is a pure function of its inputs,
 * which deterministic netplay needs. Each field boundary runs the vertical-blank work (callbacks, the
 * controllers) and yields to the host (host.c), which paces fields in real time. */

#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <libetc.h>
#include "cdrom.h"
#include "spu.h"
#include "host.h"
#include "kernel_port.h"
#include "pc_substep.h"

#define LINES_PER_FIELD_NTSC 263
#define LINES_PER_FIELD_PAL  314

static long sVideoMode = MODE_NTSC;
static unsigned long sLine;        /* lines since boot */
static unsigned long sFieldCount;  /* vertical blanks since boot */
static unsigned long sLastVSyncField;
static unsigned long sRcntBase[4];
static unsigned long sRcntReads[4];   /* reads since the last reset, the count when substepping */
static unsigned long sFrameStartLine; /* where the frame began: root counter 1's last reset */
static unsigned long sTickLines;      /* lines this frame's tick ends skipped ahead */
static int sFrameSubstepped;          /* whether this frame has run ticks before its last */
static int sLastYieldWasBlank;
static void (*sVSyncCallback)(void);

/* Interrupt handlers by interrupt number, as InterruptCallback sets them. The vertical blank's default is
 * LIBETC's own, which runs the VSyncCallback function; LIBSND chains in front of it as on the console. */
#define IRQ_VBLANK 0
#define IRQ_COUNT  11
#define DMA_COUNT  7

static void DispatchVSyncCallback(void);
static void (*sIrqHandlers[IRQ_COUNT])() = {DispatchVSyncCallback};
static void (*sDmaHandlers[DMA_COUNT])();
static unsigned sPendingDma;

static unsigned long LinesPerField(void)
{
    return sVideoMode == MODE_PAL ? LINES_PER_FIELD_PAL : LINES_PER_FIELD_NTSC;
}

static void DispatchVSyncCallback(void)
{
    if (sVSyncCallback) {
        sVSyncCallback();
    }
}

/* Raises a DMA channel's completion interrupt; it runs at the next kernel call (KernelPollInterrupts). */
void KernelRaiseDma(int channel)
{
    sPendingDma |= 1u << channel;
}

/* Runs the interrupts raised since the last call. The emulated hardware raises them while the game's code
 * runs, so they are taken where the game calls the kernel, as the console takes them between instructions. */
void KernelPollInterrupts(void)
{
    while (sPendingDma) {
        int channel = 0;
        while (!(sPendingDma & (1u << channel))) {
            channel++;
        }
        sPendingDma &= ~(1u << channel);
        if (sDmaHandlers[channel]) {
            sDmaHandlers[channel]();
        }
    }
}

static void VerticalBlank(void)
{
    sFieldCount++;
    KernelPollInterrupts();
    CdVerticalBlank();
    SpuVerticalBlank();
    if (sIrqHandlers[IRQ_VBLANK]) {
        sIrqHandlers[IRQ_VBLANK]();
    }
    sLastYieldWasBlank = 1;
    HostVerticalBlank();
}

/* Moves emulated time to `line`, running the vertical blank of every field boundary crossed. */
static void AdvanceTo(unsigned long line)
{
    unsigned long field = LinesPerField();
    while ((sLine / field) < (line / field)) {
        sLine = (sLine / field + 1) * field;
        VerticalBlank();
    }
    sLine = line;
}

int VSync(int mode)
{
    unsigned long field = LinesPerField();
    if (mode < 0) {
        return (int)sFieldCount;
    }
    if (mode == 1) {
        return (int)(sLine % field);
    }
    {
        unsigned long target = mode == 0 ? sFieldCount + 1 : sLastVSyncField + mode;
        if (target <= sFieldCount) {
            target = sFieldCount + 1;
        }
        AdvanceTo((sLine / field + (target - sFieldCount)) * field);
    }
    sLastVSyncField = sFieldCount;
    return 0;
}

int VSyncCallback(void (*func)(void))
{
    sVSyncCallback = func;
    return 0;
}

int VSyncCallbacks(int ch, void (*func)(void))
{
    (void)ch;
    (void)func;
    return 0;
}

int ResetCallback(void)
{
    sVSyncCallback = NULL;
    memset(sIrqHandlers, 0, sizeof(sIrqHandlers));
    memset(sDmaHandlers, 0, sizeof(sDmaHandlers));
    sIrqHandlers[IRQ_VBLANK] = DispatchVSyncCallback;
    sPendingDma = 0;
    return 0;
}

int StopCallback(void)
{
    return 0;
}

int RestartCallback(void)
{
    return 0;
}

int CheckCallback(void)
{
    return 0;
}

long SetVideoMode(long mode)
{
    long old = sVideoMode;
    sVideoMode = mode;
    return old;
}

long GetVideoMode(void)
{
    return sVideoMode;
}

void *DMACallback(int ch, void (*f)())
{
    void (*old)() = sDmaHandlers[ch];
    sDmaHandlers[ch] = f;
    return (void *)old;
}

void (*InterruptCallback(int irq, void (*f)()))()
{
    void (*old)() = sIrqHandlers[irq];
    sIrqHandlers[irq] = f;
    return old;
}

/* Root counters: only counter 1, horizontal blanks, is used. */

long SetRCnt(unsigned long spec, unsigned short target, long mode)
{
    (void)target;
    (void)mode;
    return ResetRCnt(spec);
}

long GetRCnt(unsigned long spec)
{
    ++sRcntReads[spec & 3];
    if (sFrameSubstepped) {
        /* The frame's time is spent: the count runs on with the reads, as on the console, but not into the
         * next field, and the lines the ticks skipped ahead don't count. */
        return (long)((sLine - sRcntBase[spec & 3] - sTickLines + sRcntReads[spec & 3]) & 0xFFFF);
    }
    AdvanceTo(sLine + 1);
    return (long)((sLine - sRcntBase[spec & 3]) & 0xFFFF);
}

long ResetRCnt(unsigned long spec)
{
    sRcntBase[spec & 3] = sLine;
    sRcntReads[spec & 3] = 0;
    if ((spec & 3) == 1) {
        sFrameStartLine = sLine;
        sTickLines = 0;
        sFrameSubstepped = 0;
    }
    return 1;
}

void KernelSubtickEnd(void)
{
    unsigned long frameLines = 2 * LinesPerField();
    unsigned long target = sFrameStartLine + frameLines * (unsigned long)(g_pcSubtick + 1) / (unsigned long)g_pcFrameTicks;
    sLastYieldWasBlank = 0;
    sFrameSubstepped = 1;
    if (target > sLine) {
        sTickLines += target - sLine;
        AdvanceTo(target);
    }
    /* A tick that ends on a vertical blank has yielded there already. */
    if (!(sLastYieldWasBlank && sLine % LinesPerField() == 0)) {
        sLastYieldWasBlank = 0;
        HostVerticalBlank();
    }
}

int KernelLastYieldWasBlank(void)
{
    return sLastYieldWasBlank;
}

void KernelPollWait(unsigned long lines)
{
    KernelPollInterrupts();
    AdvanceTo(sLine + lines);
}

unsigned long KernelLine(void)
{
    return sLine;
}

unsigned long KernelFieldCount(void)
{
    return sFieldCount;
}

long StartRCnt(unsigned long spec)
{
    (void)spec;
    return 1;
}

long StopRCnt(unsigned long spec)
{
    (void)spec;
    return 1;
}

void ChangeClearRCnt(long counter, long flag)
{
    (void)counter;
    (void)flag;
}

/* Events: a delivered event stays pending until TestEvent sees it. */

#define EVENT_MAX 32

static struct {
    unsigned long desc;
    long spec;
    int open;
    int enabled;
    int pending;
} sEvents[EVENT_MAX];

long OpenEvent(unsigned long desc, long spec, long mode, long (*func)(void))
{
    int i;
    (void)mode;
    (void)func;
    for (i = 0; i < EVENT_MAX; i++) {
        if (!sEvents[i].open) {
            sEvents[i].desc = desc;
            sEvents[i].spec = spec;
            sEvents[i].open = 1;
            sEvents[i].enabled = 0;
            sEvents[i].pending = 0;
            return 0xF1000000 | i;
        }
    }
    return -1;
}

long CloseEvent(long event)
{
    sEvents[event & (EVENT_MAX - 1)].open = 0;
    return 1;
}

long EnableEvent(long event)
{
    sEvents[event & (EVENT_MAX - 1)].enabled = 1;
    return 1;
}

long DisableEvent(long event)
{
    sEvents[event & (EVENT_MAX - 1)].enabled = 0;
    return 1;
}

long TestEvent(long event)
{
    int i = event & (EVENT_MAX - 1);
    KernelPollInterrupts();
    if (sEvents[i].pending) {
        sEvents[i].pending = 0;
        return 1;
    }
    return 0;
}

long WaitEvent(long event)
{
    while (!TestEvent(event)) {
        VSync(0);
    }
    return 1;
}

void DeliverEvent(unsigned long desc, long spec)
{
    int i;
    for (i = 0; i < EVENT_MAX; i++) {
        if (sEvents[i].open && sEvents[i].desc == desc && sEvents[i].spec == spec) {
            sEvents[i].pending = 1;
        }
    }
}

void UnDeliverEvent(unsigned long desc, long spec)
{
    int i;
    for (i = 0; i < EVENT_MAX; i++) {
        if (sEvents[i].open && sEvents[i].desc == desc && sEvents[i].spec == spec) {
            sEvents[i].pending = 0;
        }
    }
}

long EnterCriticalSection(void)
{
    return 1;
}

void ExitCriticalSection(void)
{
}

void SysEnqIntRP(int priority, void *chain)
{
    (void)priority;
    (void)chain;
}

void SysDeqIntRP(int priority, void *chain)
{
    (void)priority;
    (void)chain;
}

void FlushCache(void)
{
}

void SetSp(unsigned long sp)
{
    (void)sp;
}

void SetMem(int megabytes)
{
    (void)megabytes;
}

/* The controllers: the BIOS fills each port's receive buffer at every vertical blank. */

static unsigned char *sPadBuffers[2];
static long sPadLengths[2];

void InitPAD(char *bufA, long lenA, char *bufB, long lenB)
{
    sPadBuffers[0] = (unsigned char *)bufA;
    sPadBuffers[1] = (unsigned char *)bufB;
    sPadLengths[0] = lenA;
    sPadLengths[1] = lenB;
    memset(bufA, 0xFF, lenA);
    memset(bufB, 0xFF, lenB);
}

int StartPAD(void)
{
    return 1;
}

void StopPAD(void)
{
}

void ChangeClearPAD(long mode)
{
    (void)mode;
}

void SendPAD(unsigned char *bufA, long lenA, unsigned char *bufB, long lenB)
{
    HostSetRumbleBuffers(bufA, lenA, bufB, lenB);
}

void HostWritePadPort(int port, int connected, unsigned buttons, const unsigned char analog[4])
{
    unsigned char *buf = sPadBuffers[port];
    if (buf == NULL) {
        return;
    }
    if (!connected) {
        buf[0] = 0xFF;
        return;
    }
    buf[0] = 0x00;
    buf[1] = analog ? 0x73 : 0x41;
    buf[2] = (unsigned char)~(buttons >> 8);
    buf[3] = (unsigned char)~buttons;
    if (analog && sPadLengths[port] >= 8) {
        memcpy(&buf[4], analog, 4);
    }
}

void pollhost(void)
{
}

void _bu_init(void)
{
}
