/* PsyQ-compatible <libspu.h> for the PC port. The decompiled LIBSPU runs unchanged over the port's emulated
 * sound processor, so these records keep the kit's layouts. */

#ifndef PORT_LIBSPU_H
#define PORT_LIBSPU_H

#include "psxport.h"

#define SPU_SUCCESS      0
#define SPU_INVALID_ARGS (-3)
#define SPU_DIAG         (-2)
#define SPU_CHECK        (-1)
#define SPU_OFF          0
#define SPU_ON           1
#define SPU_CLEAR        2
#define SPU_RESET        3
#define SPU_DONT_CARE    4
#define SPU_ALL          0
#define SPU_CDONLY       5
#define SPU_VOICEONLY    6
#define SPU_CONT         7
#define SPU_BIT          8
#define SPU_NULL         0
#define SPU_ERROR        (-1L)

#define SPU_ALLCH      0xffffffL
#define SPU_KEYCH(x)   (0x1L << (x))
#define SPU_VOICECH(x) SPU_KEYCH(x)

#define SPU_VOICE_VOLL        (0x01 << 0)
#define SPU_VOICE_VOLR        (0x01 << 1)
#define SPU_VOICE_VOLMODEL    (0x01 << 2)
#define SPU_VOICE_VOLMODER    (0x01 << 3)
#define SPU_VOICE_PITCH       (0x01 << 4)
#define SPU_VOICE_NOTE        (0x01 << 5)
#define SPU_VOICE_SAMPLE_NOTE (0x01 << 6)
#define SPU_VOICE_WDSA        (0x01 << 7)
#define SPU_VOICE_ADSR_AMODE  (0x01 << 8)
#define SPU_VOICE_ADSR_SMODE  (0x01 << 9)
#define SPU_VOICE_ADSR_RMODE  (0x01 << 10)
#define SPU_VOICE_ADSR_AR     (0x01 << 11)
#define SPU_VOICE_ADSR_DR     (0x01 << 12)
#define SPU_VOICE_ADSR_SR     (0x01 << 13)
#define SPU_VOICE_ADSR_RR     (0x01 << 14)
#define SPU_VOICE_ADSR_SL     (0x01 << 15)
#define SPU_VOICE_LSAX        (0x01 << 16)
#define SPU_VOICE_ADSR_ADSR1  (0x01 << 17)
#define SPU_VOICE_ADSR_ADSR2  (0x01 << 18)

#define SPU_VOICE_DIRECT     0
#define SPU_VOICE_LINEARIncN 1
#define SPU_VOICE_LINEARIncR 2
#define SPU_VOICE_LINEARDecN 3
#define SPU_VOICE_LINEARDecR 4
#define SPU_VOICE_EXPIncN    5
#define SPU_VOICE_EXPIncR    6
#define SPU_VOICE_EXPDec     7
#define SPU_VOICE_EXPDecN    SPU_VOICE_EXPDec
#define SPU_VOICE_EXPDecR    SPU_VOICE_EXPDec

#define SPU_REV_MODE      (0x01 << 0)
#define SPU_REV_DEPTHL    (0x01 << 1)
#define SPU_REV_DEPTHR    (0x01 << 2)
#define SPU_REV_DELAYTIME (0x01 << 3)
#define SPU_REV_FEEDBACK  (0x01 << 4)

#define SPU_REV_MODE_CHECK    (-1)
#define SPU_REV_MODE_OFF      0
#define SPU_REV_MODE_ROOM     1
#define SPU_REV_MODE_STUDIO_A 2
#define SPU_REV_MODE_STUDIO_B 3
#define SPU_REV_MODE_STUDIO_C 4
#define SPU_REV_MODE_HALL     5
#define SPU_REV_MODE_SPACE    6
#define SPU_REV_MODE_ECHO     7
#define SPU_REV_MODE_DELAY    8
#define SPU_REV_MODE_PIPE     9
#define SPU_REV_MODE_MAX      10
#define SPU_REV_MODE_CLEAR_WA 0x100

#define SPU_COMMON_MVOLL     (0x01 << 0)
#define SPU_COMMON_MVOLR     (0x01 << 1)
#define SPU_COMMON_MVOLMODEL (0x01 << 2)
#define SPU_COMMON_MVOLMODER (0x01 << 3)
#define SPU_COMMON_RVOLL     (0x01 << 4)
#define SPU_COMMON_RVOLR     (0x01 << 5)
#define SPU_COMMON_CDVOLL    (0x01 << 6)
#define SPU_COMMON_CDVOLR    (0x01 << 7)
#define SPU_COMMON_CDREV     (0x01 << 8)
#define SPU_COMMON_CDMIX     (0x01 << 9)
#define SPU_COMMON_EXTVOLL   (0x01 << 10)
#define SPU_COMMON_EXTVOLR   (0x01 << 11)
#define SPU_COMMON_EXTREV    (0x01 << 12)
#define SPU_COMMON_EXTMIX    (0x01 << 13)

#define SPU_TRANSFER_BY_DMA 0L
#define SPU_TRANSFER_BY_IO  1L
#define SPU_TRANSFER_WAIT   1L
#define SPU_TRANSFER_PEEK   0L
#define SPU_TRANSFER_GLANCE SPU_TRANSFER_PEEK

#define SPU_ENV_EVENT_QUEUEING (0x01 << 0)
#define SPU_EVENT_KEY          (0x01 << 0)
#define SPU_EVENT_PITCHLFO     (0x01 << 1)
#define SPU_EVENT_NOISE        (0x01 << 2)
#define SPU_EVENT_REVERB       (0x01 << 3)

#define SPU_MALLOC_RECSIZ 8

typedef struct {
    short left;
    short right;
} SpuVolume;

typedef struct {
    unsigned long voice;
    unsigned long mask;
    SpuVolume volume;
    SpuVolume volmode;
    SpuVolume volumex;
    unsigned short pitch;
    unsigned short note;
    unsigned short sample_note;
    short envx;
    unsigned long addr;
    unsigned long loop_addr;
    long a_mode;
    long s_mode;
    long r_mode;
    unsigned short ar;
    unsigned short dr;
    unsigned short sr;
    unsigned short rr;
    unsigned short sl;
    unsigned short adsr1;
    unsigned short adsr2;
} SpuVoiceAttr;

typedef struct {
    unsigned long mask;
    long mode;
    SpuVolume depth;
    long delay;
    long feedback;
} SpuReverbAttr;

typedef struct {
    SpuVolume volume;
    long reverb;
    long mix;
} SpuExtAttr;

typedef struct {
    unsigned long mask;
    SpuVolume mvol;
    SpuVolume mvolmode;
    SpuVolume mvolx;
    SpuExtAttr cd;
    SpuExtAttr ext;
} SpuCommonAttr;

typedef struct {
    unsigned long mask;
    unsigned long queueing;
} SpuEnv;

typedef void (*SpuIRQCallbackProc)(void);
typedef void (*SpuTransferCallbackProc)(void);

void SpuInit(void);
void SpuInitHot(void);
void SpuStart(void);
void SpuQuit(void);
long SpuSetMute(long on_off);
long SpuSetReverb(long on_off);
long SpuSetReverbModeParam(SpuReverbAttr *attr);
void SpuGetReverbModeParam(SpuReverbAttr *attr);
long SpuSetReverbDepth(SpuReverbAttr *attr);
long SpuReserveReverbWorkArea(long on_off);
unsigned long SpuSetReverbVoice(long on_off, unsigned long voice_bit);
unsigned long SpuSetNoiseVoice(long on_off, unsigned long voice_bit);
long SpuSetNoiseClock(long n_clock);
long SpuClearReverbWorkArea(long mode);
unsigned long SpuWrite(unsigned char *addr, unsigned long size);
unsigned long SpuWrite0(unsigned long size);
unsigned long SpuRead(unsigned char *addr, unsigned long size);
long SpuSetTransferMode(long mode);
unsigned long SpuSetTransferStartAddr(unsigned long addr);
unsigned long SpuWritePartly(unsigned char *addr, unsigned long size);
long SpuIsTransferCompleted(long flag);
SpuTransferCallbackProc SpuSetTransferCallback(SpuTransferCallbackProc func);
long SpuSetIRQ(long on_off);
unsigned long SpuSetIRQAddr(unsigned long addr);
SpuIRQCallbackProc SpuSetIRQCallback(SpuIRQCallbackProc func);
void SpuSetVoiceAttr(SpuVoiceAttr *arg);
void SpuGetVoiceAttr(SpuVoiceAttr *arg);
void SpuSetKey(long on_off, unsigned long voice_bit);
long SpuGetKeyStatus(unsigned long voice_bit);
unsigned long SpuFlush(unsigned long ev);
void SpuSetCommonAttr(SpuCommonAttr *attr);
void SpuGetCommonAttr(SpuCommonAttr *attr);
long SpuInitMalloc(long num, char *top);
long SpuMalloc(long size);
long SpuMallocWithStartAddr(unsigned long addr, long size);
void SpuFree(unsigned long addr);

#endif
