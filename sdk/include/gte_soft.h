/* The port's software geometry transformation engine: the coprocessor's 64 registers and its commands, so the
 * game's inline GTE sequences run unchanged. Register numbers are the hardware's (data 0-31, control 32-63). */

#ifndef GTE_SOFT_H
#define GTE_SOFT_H

typedef struct GteState {
    unsigned int data[32];
    unsigned int ctrl[32];
} GteState;

extern GteState g_gte;

unsigned int GteReadData(int reg);
void GteWriteData(int reg, unsigned int value);
#define GteReadCtrl(reg) (g_gte.ctrl[(reg)])
void GteWriteCtrl(int reg, unsigned int value);
void GteExecute(unsigned int command);
/* Sub-pixel positions of the current frame's projections (gte.c), for the renderer. */
int GtePreciseLookup(unsigned int packedScreenXy, float *x, float *y);
void GtePreciseNewFrame(void);

/* Coprocessor moves, as the console's instructions name them. */
#define GTE_MTC2(value, reg) GteWriteData((reg), (unsigned int)(value))
#define GTE_MFC2(reg)        GteReadData(reg)
#define GTE_CTC2(value, reg) GteWriteCtrl((reg), (unsigned int)(value))
#define GTE_CFC2(reg)        GteReadCtrl(reg)
#define GTE_LWC2(reg, ptr)   GteWriteData((reg), *(const unsigned int *)(ptr))
#define GTE_SWC2(reg, ptr)   (*(unsigned int *)(ptr) = GteReadData(reg))

/* Command words (the low 25 bits of the COP2 instruction). */
#define GTE_RTPS  0x0180001
#define GTE_RTPT  0x0280030
#define GTE_NCLIP 0x1400006
#define GTE_OP0   0x170000C
#define GTE_OP12  0x178000C
#define GTE_DPCS  0x0780010
#define GTE_INTPL 0x0980011
#define GTE_MVMVA_RT_V0_TR   0x0480012
#define GTE_MVMVA_RT_V0_NONE 0x0486012
#define GTE_MVMVA_RT_V1_NONE 0x048E012
#define GTE_MVMVA_RT_V2_NONE 0x0496012
#define GTE_MVMVA_RT_IR_NONE 0x049E012
#define GTE_MVMVA_LL_V0_NONE 0x04A6012
#define GTE_MVMVA_LL_IR_NONE 0x04BE012
#define GTE_MVMVA_LC_IR_BK   0x04DA012
#define GTE_NCDS  0x0E80413
#define GTE_CDP   0x1280414
#define GTE_NCDT  0x0F80416
#define GTE_NCCS  0x108041B
#define GTE_CC    0x138041C
#define GTE_NCS   0x0C8041E
#define GTE_NCT   0x0D80420
#define GTE_SQR0  0x0A00428
#define GTE_SQR12 0x0A80428
#define GTE_DCPL  0x0680029
#define GTE_DPCT  0x0F8002A
#define GTE_AVSZ3 0x158002D
#define GTE_AVSZ4 0x168002E
#define GTE_GPF0  0x190003D
#define GTE_GPF12 0x198003D
#define GTE_GPL0  0x1A0003E
#define GTE_GPL12 0x1A8003E
#define GTE_NCCT  0x118043F

#endif
