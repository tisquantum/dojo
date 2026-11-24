/* PsyQ-compatible <kernel.h> for the PC port: the BIOS services the game calls, events, root counters, the
 * device file API the memory card goes through, and the controller and memory card BIOS. */

#ifndef PORT_KERNEL_H
#define PORT_KERNEL_H

#include <sys/types.h>
#include <sys/file.h>
#include "psxport.h"

/* Root counters. */
#define DescRC   0xf2000000
#define RCntCNT0 (DescRC | 0x00)
#define RCntCNT1 (DescRC | 0x01)
#define RCntCNT2 (DescRC | 0x02)
#define RCntCNT3 (DescRC | 0x03)

#define RCntIntr     0x1000
#define RCntNotar    0x0100
#define RCntStop     0x0010
#define RCntSC       0x0001
#define RCntMdINTR   0x1000
#define RCntMdNOINTR 0x2000
#define RCntMdSC     0x0001
#define RCntMdSP     0x0000
#define RCntMdFR     0x0000
#define RCntMdGATE   0x0010

/* Event classes and specs. */
#define DescMask  0xff000000
#define DescTH    DescMask
#define DescHW    0xf0000000
#define DescEV    0xf1000000
#define DescUEV   0xf3000000
#define DescSW    0xf4000000

#define HwVBLANK (DescHW | 0x01)
#define HwGPU    (DescHW | 0x02)
#define HwCdRom  (DescHW | 0x03)
#define HwDMAC   (DescHW | 0x04)
#define HwRTC0   (DescHW | 0x05)
#define HwRTC1   (DescHW | 0x06)
#define HwRTC2   (DescHW | 0x07)
#define HwCNTL   (DescHW | 0x08)
#define HwSPU    (DescHW | 0x09)
#define HwPIO    (DescHW | 0x0a)
#define HwSIO    (DescHW | 0x0b)
#define HwCPU    (DescHW | 0x10)
#define HwCARD   (DescHW | 0x11)
#define HwCARD_0 (DescHW | 0x12)
#define HwCARD_1 (DescHW | 0x13)
#define SwCARD   (DescSW | 0x01)
#define SwMATH   (DescSW | 0x02)

#define EvSpIOE    0x0004
#define EvSpIOER   0x0008
#define EvSpERROR  0x8000
#define EvSpTIMOUT 0x0100
#define EvSpNEW    0x2000
#define EvSpUNKNOWN 0x0800
#define EvSpTRAP   0x1000
#define EvSpCOMP   0x0020
#define EvSpEOF    0x0002
#define EvSpINT    0x0002
#define EvSpCLOSE  0x0003
#define EvSpACK    0x0004
#define EvSpEXEC   0x0005
#define EvSpEND    0x0100
#define EvSpCOMB   0x0100

#define EvMdINTR   0x1000
#define EvMdNOINTR 0x2000

#define EvStUNUSED  0x0000
#define EvStWAIT    0x1000
#define EvStACTIVE  0x2000
#define EvStALREADY 0x4000

struct DIRENTRY {
    char name[20];
    long attr;
    long size;
    struct DIRENTRY *next;
    long head;
    char system[4];
};

struct EXEC {
    u_long pc0;
    u_long gp0;
    u_long t_addr;
    u_long t_size;
    u_long d_addr;
    u_long d_size;
    u_long b_addr;
    u_long b_size;
    u_long s_addr;
    u_long s_size;
    u_long sp, fp, gp, ret, base;
};

struct ToT {
    u_long *head;
    long size;
};

/* Root counters. */
long SetRCnt(unsigned long spec, unsigned short target, long mode);
long GetRCnt(unsigned long spec);
long ResetRCnt(unsigned long spec);
long StartRCnt(unsigned long spec);
long StopRCnt(unsigned long spec);
void ChangeClearRCnt(long counter, long flag);

/* Events. */
long OpenEvent(unsigned long desc, long spec, long mode, long (*func)(void));
long CloseEvent(long event);
long WaitEvent(long event);
long TestEvent(long event);
long EnableEvent(long event);
long DisableEvent(long event);
void DeliverEvent(unsigned long ev1, long ev2);
void UnDeliverEvent(unsigned long ev1, long ev2);

/* Interrupts and threads. */
long EnterCriticalSection(void);
void ExitCriticalSection(void);
void SysEnqIntRP(int priority, void *chain);
void SysDeqIntRP(int priority, void *chain);
void ReturnFromException(void);
void FlushCache(void);
void SetSp(unsigned long sp);
void SetMem(int megabytes);
long Load(char *name, struct EXEC *exec);
long Exec(struct EXEC *exec, long argc, char **argv);

/* Device files (the memory card's "bu00:" and "bu10:"). Their BIOS names collide with the host C library,
 * so calls are routed to the port's versions. */
#define open(devname, flag)     PsxOpen((devname), (flag))
#define close(fd)               PsxClose(fd)
#define read(fd, buf, n)        PsxRead((fd), (buf), (n))
#define write(fd, buf, n)       PsxWrite((fd), (buf), (n))
#define lseek(fd, offset, flag) PsxLseek((fd), (offset), (flag))
#define firstfile(name, dir)    PsxFirstFile((name), (dir))
#define nextfile(dir)           PsxNextFile(dir)
#define erase(name)             PsxErase(name)
#define format(fs)              PsxFormat(fs)
#define delete(name)            PsxDelete(name)

long PsxOpen(const char *devname, unsigned long flag);
long PsxClose(long fd);
long PsxRead(long fd, void *buf, long n);
long PsxWrite(long fd, void *buf, long n);
long PsxLseek(long fd, long offset, long flag);
struct DIRENTRY *PsxFirstFile(char *name, struct DIRENTRY *dir);
struct DIRENTRY *PsxNextFile(struct DIRENTRY *dir);
long PsxErase(char *name);
long PsxFormat(char *fs);
long PsxDelete(char *name);

/* Controllers and memory card BIOS. */
void InitPAD(char *bufA, long lenA, char *bufB, long lenB);
int StartPAD(void);
void StopPAD(void);
void ChangeClearPAD(long mode);
void InitCARD(long pad_enable);
long StartCARD(void);
long StopCARD(void);
long _card_info(long chan);
long _card_load(long chan);
long _card_clear(long chan);
long _card_auto(long val);
long _card_status(long drv);
long _card_wait(long drv);
long _card_read(long chan, long block, unsigned char *buf);
long _card_write(long chan, long block, unsigned char *buf);
void _new_card(void);
void _bu_init(void);

#endif
