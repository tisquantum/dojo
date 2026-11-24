/* PsyQ-compatible inline GTE macros for the PC port, each the console's coprocessor sequence run on the
 * software GTE. Arguments follow the kit's conventions: ld* of a scalar take the value, the rest pointers. */

#ifndef PORT_INLINE_C_H
#define PORT_INLINE_C_H

#include "gte_soft.h"

#define gte_nop() ((void)0)

/* Loads. */
#define gte_ldv0(r0)                                                                                          \
    (GTE_LWC2(0, &((const short *)(r0))[0]), GTE_LWC2(1, &((const short *)(r0))[2]))
#define gte_ldv1(r0)                                                                                          \
    (GTE_LWC2(2, &((const short *)(r0))[0]), GTE_LWC2(3, &((const short *)(r0))[2]))
#define gte_ldv2(r0)                                                                                          \
    (GTE_LWC2(4, &((const short *)(r0))[0]), GTE_LWC2(5, &((const short *)(r0))[2]))
#define gte_ldv3(r0, r1, r2) (gte_ldv0(r0), gte_ldv1(r1), gte_ldv2(r2))
#define gte_ldlv0(r0)                                                                                         \
    (GTE_MTC2((((const long *)(r0))[0] & 0xFFFF) | (((const long *)(r0))[1] << 16), 0),                       \
     GTE_MTC2(((const long *)(r0))[2], 1))
#define gte_ldlvl(r0)                                                                                         \
    (GTE_LWC2(9, &((const long *)(r0))[0]), GTE_LWC2(10, &((const long *)(r0))[1]),                          \
     GTE_LWC2(11, &((const long *)(r0))[2]))
#define gte_ldsv(r0)                                                                                          \
    (GTE_MTC2(((const short *)(r0))[0], 9), GTE_MTC2(((const short *)(r0))[1], 10),                          \
     GTE_MTC2(((const short *)(r0))[2], 11))
#define gte_ldrgb(r0)          GTE_LWC2(6, (r0))
#define gte_ldlzc(r0)          GTE_MTC2((r0), 30)
#define gte_lddp(r0)           GTE_MTC2((r0), 8)
#define gte_ldsxy3(r0, r1, r2) (GTE_MTC2((r0), 12), GTE_MTC2((r1), 13), GTE_MTC2((r2), 14))
#define gte_ldsz3(r0, r1, r2)  (GTE_MTC2((r0), 17), GTE_MTC2((r1), 18), GTE_MTC2((r2), 19))
#define gte_ldsz4(r0, r1, r2, r3)                                                                             \
    (GTE_MTC2((r0), 16), GTE_MTC2((r1), 17), GTE_MTC2((r2), 18), GTE_MTC2((r3), 19))
#define gte_ldopv1(r0)                                                                                        \
    (GTE_CTC2(((const long *)(r0))[0], 0), GTE_CTC2(((const long *)(r0))[1], 2),                             \
     GTE_CTC2(((const long *)(r0))[2], 4))
#define gte_ldopv2(r0) gte_ldlvl(r0)

#define gte_SetRotMatrix(r0)                                                                                  \
    (GTE_CTC2(((const unsigned int *)(r0))[0], 0), GTE_CTC2(((const unsigned int *)(r0))[1], 1),             \
     GTE_CTC2(((const unsigned int *)(r0))[2], 2), GTE_CTC2(((const unsigned int *)(r0))[3], 3),             \
     GTE_CTC2(((const unsigned int *)(r0))[4], 4))
#define gte_SetLightMatrix(r0)                                                                                \
    (GTE_CTC2(((const unsigned int *)(r0))[0], 8), GTE_CTC2(((const unsigned int *)(r0))[1], 9),             \
     GTE_CTC2(((const unsigned int *)(r0))[2], 10), GTE_CTC2(((const unsigned int *)(r0))[3], 11),           \
     GTE_CTC2(((const unsigned int *)(r0))[4], 12))
#define gte_SetColorMatrix(r0)                                                                                \
    (GTE_CTC2(((const unsigned int *)(r0))[0], 16), GTE_CTC2(((const unsigned int *)(r0))[1], 17),           \
     GTE_CTC2(((const unsigned int *)(r0))[2], 18), GTE_CTC2(((const unsigned int *)(r0))[3], 19),           \
     GTE_CTC2(((const unsigned int *)(r0))[4], 20))
#define gte_SetTransMatrix(r0)                                                                                \
    (GTE_CTC2(((const MATRIX *)(r0))->t[0], 5), GTE_CTC2(((const MATRIX *)(r0))->t[1], 6),                   \
     GTE_CTC2(((const MATRIX *)(r0))->t[2], 7))
#define gte_SetTransVector(r0)                                                                                \
    (GTE_CTC2(((const long *)(r0))[0], 5), GTE_CTC2(((const long *)(r0))[1], 6),                             \
     GTE_CTC2(((const long *)(r0))[2], 7))
#define gte_SetBackColor(r, g, b) (GTE_CTC2((r) << 4, 13), GTE_CTC2((g) << 4, 14), GTE_CTC2((b) << 4, 15))
#define gte_SetFarColor(r, g, b)  (GTE_CTC2((r) << 4, 21), GTE_CTC2((g) << 4, 22), GTE_CTC2((b) << 4, 23))

/* Commands. */
#define gte_rtps()  GteExecute(GTE_RTPS)
#define gte_rtpt()  GteExecute(GTE_RTPT)
#define gte_nclip() GteExecute(GTE_NCLIP)
#define gte_avsz3() GteExecute(GTE_AVSZ3)
#define gte_avsz4() GteExecute(GTE_AVSZ4)
#define gte_op0()   GteExecute(GTE_OP0)
#define gte_op12()  GteExecute(GTE_OP12)
#define gte_sqr0()  GteExecute(GTE_SQR0)
#define gte_sqr12() GteExecute(GTE_SQR12)
#define gte_gpf0()  GteExecute(GTE_GPF0)
#define gte_gpf12() GteExecute(GTE_GPF12)
#define gte_gpl0()  GteExecute(GTE_GPL0)
#define gte_gpl12() GteExecute(GTE_GPL12)
#define gte_rt()    GteExecute(GTE_MVMVA_RT_V0_TR)
#define gte_rtv0()  GteExecute(GTE_MVMVA_RT_V0_NONE)
#define gte_rtv1()  GteExecute(GTE_MVMVA_RT_V1_NONE)
#define gte_rtv2()  GteExecute(GTE_MVMVA_RT_V2_NONE)
#define gte_rtir()  GteExecute(GTE_MVMVA_RT_IR_NONE)
#define gte_ll()    GteExecute(GTE_MVMVA_LL_V0_NONE)
#define gte_nct()   GteExecute(GTE_NCT)
#define gte_ncs()   GteExecute(GTE_NCS)
#define gte_nccs()  GteExecute(GTE_NCCS)
#define gte_ncct()  GteExecute(GTE_NCCT)
#define gte_ncds()  GteExecute(GTE_NCDS)
#define gte_ncdt()  GteExecute(GTE_NCDT)
#define gte_cc()    GteExecute(GTE_CC)
#define gte_cdp()   GteExecute(GTE_CDP)
#define gte_dpcs()  GteExecute(GTE_DPCS)
#define gte_dpct()  GteExecute(GTE_DPCT)
#define gte_dcpl()  GteExecute(GTE_DCPL)
#define gte_intpl() GteExecute(GTE_INTPL)

/* Stores. */
#define gte_stsxy(r0)  GTE_SWC2(14, (r0))
#define gte_stsxy0(r0) GTE_SWC2(12, (r0))
#define gte_stsxy1(r0) GTE_SWC2(13, (r0))
#define gte_stsxy2(r0) GTE_SWC2(14, (r0))
#define gte_stsxy3(r0, r1, r2) (GTE_SWC2(12, (r0)), GTE_SWC2(13, (r1)), GTE_SWC2(14, (r2)))
#define gte_stsz(r0)   GTE_SWC2(19, (r0))
/* Three swc2 on the console: whole words, the depths zero-extended. (A 16-bit store left the upper half of
 * weather.c's long slots as it was, and its snow sized flakes from that and drew long streaks.) */
#define gte_stsz3(r0, r1, r2) (GTE_SWC2(17, (r0)), GTE_SWC2(18, (r1)), GTE_SWC2(19, (r2)))
#define gte_stotz(r0)    (*(long *)(r0) = (long)GTE_MFC2(7))
#define gte_stopz(r0)    (*(long *)(r0) = (long)GTE_MFC2(24))
#define gte_stszotz(r0)  (*(long *)(r0) = (long)GTE_MFC2(19) >> 2)
#define gte_stlzc(r0)    (*(long *)(r0) = (long)GTE_MFC2(31))
#define gte_stdp(r0)     (*(long *)(r0) = (long)GTE_MFC2(8))
#define gte_stflg(r0)    (*(long *)(r0) = (long)GTE_CFC2(31))
#define gte_strgb(r0)    GTE_SWC2(22, (r0))
#define gte_strgb3(r0, r1, r2) (GTE_SWC2(20, (r0)), GTE_SWC2(21, (r1)), GTE_SWC2(22, (r2)))
#define gte_stlvl(r0)                                                                                         \
    (GTE_SWC2(9, &((long *)(r0))[0]), GTE_SWC2(10, &((long *)(r0))[1]), GTE_SWC2(11, &((long *)(r0))[2]))
#define gte_stlvnl(r0)                                                                                        \
    (GTE_SWC2(25, &((long *)(r0))[0]), GTE_SWC2(26, &((long *)(r0))[1]), GTE_SWC2(27, &((long *)(r0))[2]))
#define gte_stsv(r0)                                                                                          \
    (((short *)(r0))[0] = (short)GTE_MFC2(9), ((short *)(r0))[1] = (short)GTE_MFC2(10),                       \
     ((short *)(r0))[2] = (short)GTE_MFC2(11))

#endif
