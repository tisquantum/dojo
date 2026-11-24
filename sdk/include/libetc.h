/* PsyQ-compatible <libetc.h> for the PC port: vertical blank, callbacks, video mode and the simple pad API. */

#ifndef PORT_LIBETC_H
#define PORT_LIBETC_H

#include <sys/types.h>
#include "psxport.h"

#define MODE_NTSC 0
#define MODE_PAL  1

/* Pad buttons as PadRead reports them; _PAD(1, b) moves a button to the second controller's half. */
#define PADLup     (1 << 12)
#define PADLright  (1 << 13)
#define PADLdown   (1 << 14)
#define PADLleft   (1 << 15)
#define PADRup     (1 << 4)
#define PADRright  (1 << 5)
#define PADRdown   (1 << 6)
#define PADRleft   (1 << 7)
#define PADL1      (1 << 2)
#define PADL2      (1 << 0)
#define PADR1      (1 << 3)
#define PADR2      (1 << 1)
#define PADstart   (1 << 11)
#define PADselect  (1 << 8)
#define PADi       (1 << 9)
#define PADj       (1 << 10)
#define _PAD(x, y) ((y) << ((x) << 4))

extern int PadIdentifier;

int VSync(int mode);
int VSyncCallback(void (*func)(void));
int VSyncCallbacks(int ch, void (*func)(void));
int ResetCallback(void);
int StopCallback(void);
int RestartCallback(void);
int CheckCallback(void);
long SetVideoMode(long mode);
long GetVideoMode(void);
void PadInit(int mode);
u_long PadRead(int id);
void PadStop(void);
void *DMACallback(int ch, void (*f)());
void (*InterruptCallback(int irq, void (*f)()))();

#endif
