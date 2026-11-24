/* PsyQ-compatible <libcd.h> for the PC port. The game drives the drive at command level, so the port
 * implements libcd's commands over a virtual drive reading the player's disc image. */

#ifndef PORT_LIBCD_H
#define PORT_LIBCD_H

#include <sys/types.h>
#include "psxport.h"

/* Commands. */
#define CdlNop       0x01
#define CdlSetloc    0x02
#define CdlPlay      0x03
#define CdlForward   0x04
#define CdlBackward  0x05
#define CdlReadN     0x06
#define CdlStandby   0x07
#define CdlStop      0x08
#define CdlPause     0x09
#define CdlReset     0x0a
#define CdlMute      0x0b
#define CdlDemute    0x0c
#define CdlSetfilter 0x0d
#define CdlSetmode   0x0e
#define CdlGetparam  0x0f
#define CdlGetlocL   0x10
#define CdlGetlocP   0x11
#define CdlGetTN     0x13
#define CdlGetTD     0x14
#define CdlSeekL     0x15
#define CdlSeekP     0x16
#define CdlReadS     0x1b

/* Interrupt results. */
#define CdlNoIntr      0x00
#define CdlDataReady   0x01
#define CdlComplete    0x02
#define CdlAcknowledge 0x03
#define CdlDataEnd     0x04
#define CdlDiskError   0x05

/* Status bits. */
#define CdlStatPlay      0x80
#define CdlStatSeek      0x40
#define CdlStatRead      0x20
#define CdlStatShellOpen 0x10
#define CdlStatSeekError 0x04
#define CdlStatStandby   0x02
#define CdlStatError     0x01

/* Modes. */
#define CdlModeStream  0x100
#define CdlModeStream2 0x120
#define CdlModeSpeed   0x80
#define CdlModeRT      0x40
#define CdlModeSize1   0x20
#define CdlModeSize0   0x10
#define CdlModeSF      0x08
#define CdlModeRept    0x04
#define CdlModeAP      0x02
#define CdlModeDA      0x01

#define CdlMAXFILE   64
#define CdlMAXDIR    128
#define CdlMAXLEVEL  8

typedef struct {
    u_char minute;
    u_char second;
    u_char sector;
    u_char track;
} CdlLOC;

typedef struct {
    u_char val0;
    u_char val1;
    u_char val2;
    u_char val3;
} CdlATV;

typedef struct {
    CdlLOC pos;
    u_long size;
    char name[16];
} CdlFILE;

typedef struct {
    u_char file;
    u_char chan;
    u_short pad;
} CdlFILTER;

typedef void (*CdlCB)(u_char, u_char *);

typedef struct {
    u_short id;
    u_short type;
    u_short secCount;
    u_short nSectors;
    u_long frameCount;
    u_long frameSize;
    u_short width;
    u_short height;
    u_long dummy1;
    u_long dummy2;
    CdlLOC loc;
} StHEADER;

#define btoi(b) ((b) / 16 * 10 + (b) % 16)
#define itob(i) ((i) / 10 * 16 + (i) % 10)

int CdInit(void);
int CdReset(int mode);
void CdFlush(void);
int CdSetDebug(int level);
char *CdComstr(u_char com);
char *CdIntstr(u_char intr);
int CdSync(int mode, u_char *result);
int CdReady(int mode, u_char *result);
CdlCB CdSyncCallback(CdlCB func);
CdlCB CdReadyCallback(CdlCB func);
int CdControl(u_char com, u_char *param, u_char *result);
int CdControlB(u_char com, u_char *param, u_char *result);
int CdControlF(u_char com, u_char *param);
int CdMix(CdlATV *vol);
int CdGetSector(void *madr, int size);
void *CdDataCallback(void (*func)(void));
CdlLOC *CdIntToPos(int i, CdlLOC *p);
int CdPosToInt(CdlLOC *p);
CdlFILE *CdSearchFile(CdlFILE *fp, char *name);
int CdRead(int sectors, u_long *buf, int mode);
int CdRead2(long mode);
int CdReadSync(int mode, u_char *result);
CdlCB CdReadCallback(CdlCB func);
int CdStatus(void);
int CdMode(void);
int CdLastCom(void);

void StSetRing(u_long *ring_addr, u_long ring_size);
void StClearRing(void);
void StUnSetRing(void);
void StSetStream(u_long mode, u_long start_frame, u_long end_frame, void (*func1)(void), void (*func2)(void));
void StSetEmulate(u_long *addr, u_long mode, u_long start_frame, u_long end_frame, void (*func1)(void),
                  void (*func2)(void));
u_long StFreeRing(u_long *base);
u_long StGetNext(u_long **addr, u_long **header);
u_long StGetNextS(u_long **addr, u_long **header);
void StSetMask(u_long mask, u_long start, u_long end);
void StCdInterrupt(void);
int StGetBackloc(CdlLOC *loc);

extern u_long StCdIntrFlag;

#endif
