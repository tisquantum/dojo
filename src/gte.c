/* The software geometry transformation engine: the coprocessor's registers, their read and write quirks, its
 * commands with the hardware's saturation and flag behaviour, and its division. Written from the documented
 * behaviour of the console's GTE so the game's fixed-point results come out as on the console. */

#include <stdint.h>
#include "gte_soft.h"

GteState g_gte;

#define D(n) g_gte.data[n]
#define C(n) g_gte.ctrl[n]

/* Data registers. */
enum {
    R_VXY0, R_VZ0, R_VXY1, R_VZ1, R_VXY2, R_VZ2, R_RGBC, R_OTZ,
    R_IR0, R_IR1, R_IR2, R_IR3, R_SXY0, R_SXY1, R_SXY2, R_SXYP,
    R_SZ0, R_SZ1, R_SZ2, R_SZ3, R_RGB0, R_RGB1, R_RGB2, R_RES1,
    R_MAC0, R_MAC1, R_MAC2, R_MAC3, R_IRGB, R_ORGB, R_LZCS, R_LZCR
};

/* Control registers. */
enum {
    C_RT0, C_RT1, C_RT2, C_RT3, C_RT4, C_TRX, C_TRY, C_TRZ,
    C_L0, C_L1, C_L2, C_L3, C_L4, C_RBK, C_GBK, C_BBK,
    C_LR0, C_LR1, C_LR2, C_LR3, C_LR4, C_RFC, C_GFC, C_BFC,
    C_OFX, C_OFY, C_H, C_DQA, C_DQB, C_ZSF3, C_ZSF4, C_FLAG
};

#define FLAG_MAC1_POS (1u << 30)
#define FLAG_MAC2_POS (1u << 29)
#define FLAG_MAC3_POS (1u << 28)
#define FLAG_MAC1_NEG (1u << 27)
#define FLAG_MAC2_NEG (1u << 26)
#define FLAG_MAC3_NEG (1u << 25)
#define FLAG_IR1      (1u << 24)
#define FLAG_IR2      (1u << 23)
#define FLAG_IR3      (1u << 22)
#define FLAG_R        (1u << 21)
#define FLAG_G        (1u << 20)
#define FLAG_B        (1u << 19)
#define FLAG_SZ       (1u << 18)
#define FLAG_DIV      (1u << 17)
#define FLAG_MAC0_POS (1u << 16)
#define FLAG_MAC0_NEG (1u << 15)
#define FLAG_SX       (1u << 14)
#define FLAG_SY       (1u << 13)
#define FLAG_IR0      (1u << 12)

static uint32_t sFlag;

static int16_t Lo(uint32_t w) { return (int16_t)(w & 0xFFFF); }
static int16_t Hi(uint32_t w) { return (int16_t)(w >> 16); }

/* A 3x3 matrix of the control block, starting at control register `base`: RT at 0, LLM at 8, LCM at 16. */
static int16_t MatrixElement(int base, int row, int col)
{
    int index = row * 3 + col;
    uint32_t w = C(base + index / 2);
    return (index & 1) ? Hi(w) : Lo(w);
}

static int64_t CheckMac(int n, int64_t value)
{
    static const uint32_t pos[4] = {0, FLAG_MAC1_POS, FLAG_MAC2_POS, FLAG_MAC3_POS};
    static const uint32_t neg[4] = {0, FLAG_MAC1_NEG, FLAG_MAC2_NEG, FLAG_MAC3_NEG};
    if (value > 0x7FFFFFFFFFFLL) {
        sFlag |= pos[n];
    } else if (value < -0x80000000000LL) {
        sFlag |= neg[n];
    }
    /* The accumulator is 44 bits wide; wrap as the hardware does. */
    return (int64_t)((uint64_t)value << 20) >> 20;
}

static int32_t SetMac(int n, int64_t value, int shift)
{
    value = CheckMac(n, value) >> shift;
    D(R_MAC1 + n - 1) = (uint32_t)(int32_t)value;
    return (int32_t)value;
}

static int32_t SetMac0(int64_t value)
{
    if (value > 0x7FFFFFFFLL) {
        sFlag |= FLAG_MAC0_POS;
    } else if (value < -0x80000000LL) {
        sFlag |= FLAG_MAC0_NEG;
    }
    D(R_MAC0) = (uint32_t)(int32_t)value;
    return (int32_t)value;
}

static void SetIr(int n, int32_t value, int lm)
{
    static const uint32_t bit[4] = {0, FLAG_IR1, FLAG_IR2, FLAG_IR3};
    int32_t lo = lm ? 0 : -0x8000;
    if (value < lo) {
        value = lo;
        sFlag |= bit[n];
    } else if (value > 0x7FFF) {
        value = 0x7FFF;
        sFlag |= bit[n];
    }
    D(R_IR0 + n) = (uint32_t)(int32_t)value;
}

/* MAC1-3 into IR1-3; with sf=0 the hardware sets IR3's flag from MAC3 >> 12 while saturating MAC3 itself. */
static void MacToIr(int lm, int sf, int64_t mac3Unshifted)
{
    SetIr(1, (int32_t)D(R_MAC1), lm);
    SetIr(2, (int32_t)D(R_MAC2), lm);
    if (sf == 0) {
        int32_t flagValue = (int32_t)(mac3Unshifted >> 12);
        int32_t value = (int32_t)D(R_MAC3);
        int32_t lo = lm ? 0 : -0x8000;
        if (flagValue < -0x8000 || flagValue > 0x7FFF) {
            sFlag |= FLAG_IR3;
        }
        if (value < lo) {
            value = lo;
        } else if (value > 0x7FFF) {
            value = 0x7FFF;
        }
        D(R_IR3) = (uint32_t)value;
    } else {
        SetIr(3, (int32_t)D(R_MAC3), lm);
    }
}

static uint32_t ColorChannel(int32_t value, uint32_t flag)
{
    if (value < 0) {
        sFlag |= flag;
        return 0;
    }
    if (value > 0xFF) {
        sFlag |= flag;
        return 0xFF;
    }
    return (uint32_t)value;
}

static void PushColor(void)
{
    uint32_t r = ColorChannel((int32_t)D(R_MAC1) >> 4, FLAG_R);
    uint32_t g = ColorChannel((int32_t)D(R_MAC2) >> 4, FLAG_G);
    uint32_t b = ColorChannel((int32_t)D(R_MAC3) >> 4, FLAG_B);
    D(R_RGB0) = D(R_RGB1);
    D(R_RGB1) = D(R_RGB2);
    D(R_RGB2) = r | (g << 8) | (b << 16) | (D(R_RGBC) & 0xFF000000u);
}

static void PushSz(int32_t value)
{
    if (value < 0) {
        value = 0;
        sFlag |= FLAG_SZ;
    } else if (value > 0xFFFF) {
        value = 0xFFFF;
        sFlag |= FLAG_SZ;
    }
    D(R_SZ0) = D(R_SZ1);
    D(R_SZ1) = D(R_SZ2);
    D(R_SZ2) = D(R_SZ3);
    D(R_SZ3) = (uint32_t)value;
}

static int32_t LimitScreen(int32_t value, uint32_t flag)
{
    if (value < -0x400) {
        sFlag |= flag;
        return -0x400;
    }
    if (value > 0x3FF) {
        sFlag |= flag;
        return 0x3FF;
    }
    return value;
}

static void PushSxy(int32_t x, int32_t y)
{
    D(R_SXY0) = D(R_SXY1);
    D(R_SXY1) = D(R_SXY2);
    D(R_SXY2) = ((uint32_t)x & 0xFFFF) | ((uint32_t)y << 16);
}

static uint8_t sUnrTable[0x101];
static int sUnrReady;

static void InitUnrTable(void)
{
    int i;
    for (i = 0; i < 0x101; i++) {
        int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
        sUnrTable[i] = (uint8_t)(v < 0 ? 0 : v);
    }
    sUnrReady = 1;
}

/* The hardware's Newton-Raphson reciprocal: H / SZ3 in 1.16. */
static uint32_t Divide(uint32_t h, uint32_t sz3)
{
    uint32_t n, d, u;
    int z;

    if (h >= sz3 * 2) {
        sFlag |= FLAG_DIV;
        return 0x1FFFF;
    }
    if (!sUnrReady) {
        InitUnrTable();
    }
    z = 0;
    while (z < 16 && !(sz3 & (0x8000u >> z))) {
        z++;
    }
    n = h << z;
    d = sz3 << z;
    u = sUnrTable[(d - 0x7FC0) >> 7] + 0x101;
    d = (0x2000080 - d * u) >> 8;
    d = (0x0000080 + d * u) >> 8;
    {
        uint64_t q = ((uint64_t)n * d + 0x8000) >> 16;
        return q > 0x1FFFF ? 0x1FFFF : (uint32_t)q;
    }
}

static void VertexOf(int v, int32_t out[3])
{
    if (v == 3) {
        out[0] = (int16_t)D(R_IR1);
        out[1] = (int16_t)D(R_IR2);
        out[2] = (int16_t)D(R_IR3);
        return;
    }
    out[0] = Lo(D(R_VXY0 + v * 2));
    out[1] = Hi(D(R_VXY0 + v * 2));
    out[2] = Lo(D(R_VZ0 + v * 2));
}

/* Perspective transformation of vertex v; `last` also produces the depth cue IR0. */
/* Sub-pixel precision for the renderer. The GTE rounds a projected vertex to whole pixels; each projection
 * also records the exact position under the packed word it produced, and a renderer drawing a packet looks
 * its vertex words up (GtePreciseLookup). A word the game computed itself finds nothing and stays whole;
 * two projections that round to the same pixel share an entry, wrong by under a pixel. It is a table of
 * the current frame's projections, emptied by GtePreciseNewFrame; nothing the game reads depends on it. */
#define PRECISE_SLOTS 65536
#define PRECISE_PROBES 8

typedef struct Precise {
    uint32_t key;
    uint32_t generation;
    float x, y;
} Precise;

/* For drawing only, so not part of the simulation's save state. */
#pragma clang section bss = ".bss$render" data = ".data$render"
static Precise sPrecise[PRECISE_SLOTS];
static uint32_t sPreciseGeneration = 1;
#pragma clang section bss = ".bss$sim" data = ".data$sim"

static uint32_t PreciseHash(uint32_t key)
{
    return (key * 2654435761u) >> 16;
}

static void RecordPrecise(uint32_t key, float x, float y)
{
    uint32_t h = PreciseHash(key), i;
    for (i = 0; i < PRECISE_PROBES; i++) {
        Precise *p = &sPrecise[(h + i) & (PRECISE_SLOTS - 1)];
        if (p->generation != sPreciseGeneration || p->key == key) {
            p->key = key;
            p->generation = sPreciseGeneration;
            p->x = x;
            p->y = y;
            return;
        }
    }
}

int GtePreciseLookup(uint32_t key, float *x, float *y)
{
    uint32_t h = PreciseHash(key), i;
    for (i = 0; i < PRECISE_PROBES; i++) {
        const Precise *p = &sPrecise[(h + i) & (PRECISE_SLOTS - 1)];
        if (p->generation != sPreciseGeneration) {
            return 0;
        }
        if (p->key == key) {
            *x = p->x;
            *y = p->y;
            return 1;
        }
    }
    return 0;
}

void GtePreciseNewFrame(void)
{
    sPreciseGeneration++;
}

static void TransformPerspective(int v, int sf, int lm, int last)
{
    int32_t vec[3];
    int64_t mac3;
    int64_t view[3];
    uint32_t q;
    int64_t sx, sy;
    int shift = sf * 12;
    int i;

    VertexOf(v, vec);
    for (i = 0; i < 3; i++) {
        int64_t acc = (int64_t)(int32_t)C(C_TRX + i) << 12;
        acc = CheckMac(i + 1, acc + (int64_t)MatrixElement(C_RT0, i, 0) * vec[0]);
        acc = CheckMac(i + 1, acc + (int64_t)MatrixElement(C_RT0, i, 1) * vec[1]);
        acc = acc + (int64_t)MatrixElement(C_RT0, i, 2) * vec[2];
        if (i == 2) {
            mac3 = CheckMac(3, acc);
        }
        view[i] = acc;
        SetMac(i + 1, acc, shift);
    }
    MacToIr(lm, sf, mac3);
    PushSz((int32_t)(mac3 >> 12));

    q = Divide((uint16_t)C(C_H), D(R_SZ3));
    sx = (int64_t)(int32_t)C(C_OFX) + (int64_t)(int16_t)D(R_IR1) * q;
    SetMac0(sx);
    sy = (int64_t)(int32_t)C(C_OFY) + (int64_t)(int16_t)D(R_IR2) * q;
    SetMac0(sy);
    PushSxy(LimitScreen((int32_t)(sx >> 16), FLAG_SX), LimitScreen((int32_t)(sy >> 16), FLAG_SY));
    {
        /* The same projection unrounded, from the accumulators before any shift; not for a vertex the
         * divide or the screen clamp saturated, whose whole position is not a projection's. */
        int32_t wx = (int32_t)(sx >> 16), wy = (int32_t)(sy >> 16);
        if (q != 0x1FFFF && view[2] > 0 && wx >= -0x400 && wx <= 0x3FF && wy >= -0x400 && wy <= 0x3FF) {
            double h = (double)(uint16_t)C(C_H);
            double px = (int32_t)C(C_OFX) / 65536.0 + (double)view[0] * h / (double)view[2];
            double py = (int32_t)C(C_OFY) / 65536.0 + (double)view[1] * h / (double)view[2];
            /* The console's own result strays from the exact one by up to a pixel or so where the game scales
             * coordinates down (the wobble this removes); farther than two, the entry would not be this
             * vertex's, so it is not kept. */
            if (px > wx - 2.0 && px < wx + 3.0 && py > wy - 2.0 && py < wy + 3.0) {
                RecordPrecise(D(R_SXY2), (float)px, (float)py);
            }
        }
    }

    if (last) {
        int64_t dq = (int64_t)(int16_t)C(C_DQA) * q + (int32_t)C(C_DQB);
        int32_t ir0;
        SetMac0(dq);
        ir0 = (int32_t)(dq >> 12);
        if (ir0 < 0) {
            ir0 = 0;
            sFlag |= FLAG_IR0;
        } else if (ir0 > 0x1000) {
            ir0 = 0x1000;
            sFlag |= FLAG_IR0;
        }
        D(R_IR0) = (uint32_t)ir0;
    }
}

/* MAC = (cv << 12 + M * v) >> shift; IR = lim(MAC). The far-colour vector reproduces the hardware's bug. */
static void MultiplyMatrixVector(int mx, int v, int cv, int sf, int lm)
{
    int32_t vec[3];
    int shift = sf * 12;
    int i;

    VertexOf(v, vec);
    for (i = 0; i < 3; i++) {
        int16_t m0, m1, m2;
        int64_t base, acc;
        if (mx == 3) {
            /* The hardware's garbage matrix: [-R*16, R*16, IR0], then RT13 and RT22 across. */
            int16_t red = (int16_t)((D(R_RGBC) & 0xFF) << 4);
            int16_t r13 = MatrixElement(C_RT0, 0, 2);
            int16_t r22 = MatrixElement(C_RT0, 1, 1);
            if (i == 0) {
                m0 = (int16_t)-red;
                m1 = red;
                m2 = (int16_t)D(R_IR0);
            } else {
                m0 = m1 = m2 = (i == 1) ? r13 : r22;
            }
        } else {
            int base = mx == 0 ? C_RT0 : (mx == 1 ? C_L0 : C_LR0);
            m0 = MatrixElement(base, i, 0);
            m1 = MatrixElement(base, i, 1);
            m2 = MatrixElement(base, i, 2);
        }
        switch (cv) {
        case 0:
            base = (int64_t)(int32_t)C(C_TRX + i) << 12;
            break;
        case 1:
            base = (int64_t)(int32_t)C(C_RBK + i) << 12;
            break;
        case 2:
            base = (int64_t)(int32_t)C(C_RFC + i) << 12;
            break;
        default:
            base = 0;
            break;
        }
        if (cv == 2) {
            /* The far colour and the first column only set flags; the result omits them. */
            int64_t flagOnly = CheckMac(i + 1, base + (int64_t)m0 * vec[0]);
            SetIr(i + 1, (int32_t)(flagOnly >> shift), 0);
            acc = CheckMac(i + 1, (int64_t)m1 * vec[1]);
            acc = acc + (int64_t)m2 * vec[2];
        } else {
            acc = CheckMac(i + 1, base + (int64_t)m0 * vec[0]);
            acc = CheckMac(i + 1, acc + (int64_t)m1 * vec[1]);
            acc = acc + (int64_t)m2 * vec[2];
        }
        SetMac(i + 1, acc, shift);
    }
    MacToIr(lm, sf, (int64_t)(int32_t)D(R_MAC3) << shift);
}

/* The colour of RGBC times IR, shifted up 4 into MAC. */
static void ColorTimesIr(void)
{
    uint32_t rgbc = D(R_RGBC);
    SetMac(1, ((int64_t)(rgbc & 0xFF) * (int16_t)D(R_IR1)) << 4, 0);
    SetMac(2, ((int64_t)((rgbc >> 8) & 0xFF) * (int16_t)D(R_IR2)) << 4, 0);
    SetMac(3, ((int64_t)((rgbc >> 16) & 0xFF) * (int16_t)D(R_IR3)) << 4, 0);
}

/* Interpolate MAC toward the far colour by IR0, then IR = lim(MAC) and push the colour. */
static void DepthCue(int sf, int lm)
{
    int shift = sf * 12;
    int32_t mac[3];
    int i;

    for (i = 0; i < 3; i++) {
        mac[i] = (int32_t)D(R_MAC1 + i);
    }
    for (i = 0; i < 3; i++) {
        int64_t fc = (int64_t)(int32_t)C(C_RFC + i) << 12;
        SetIr(i + 1, SetMac(i + 1, fc - mac[i], shift), 0);
    }
    for (i = 0; i < 3; i++) {
        int64_t acc = (int64_t)(int16_t)D(R_IR1 + i) * (int16_t)D(R_IR0) + mac[i];
        SetMac(i + 1, acc, shift);
    }
    MacToIr(lm, sf, (int64_t)(int32_t)D(R_MAC3) << shift);
    PushColor();
}

static void ShiftedColorFinish(int sf, int lm)
{
    int shift = sf * 12;
    SetMac(1, (int64_t)(int32_t)D(R_MAC1), shift);
    SetMac(2, (int64_t)(int32_t)D(R_MAC2), shift);
    SetMac(3, (int64_t)(int32_t)D(R_MAC3), shift);
    MacToIr(lm, sf, (int64_t)(int32_t)D(R_MAC3) << shift);
    PushColor();
}

enum { LIGHT_NC, LIGHT_NCC, LIGHT_NCD };

static void NormalColorVertex(int v, int kind, int sf, int lm)
{
    MultiplyMatrixVector(1, v, 3, sf, lm);
    MultiplyMatrixVector(2, 3, 1, sf, lm);
    if (kind == LIGHT_NC) {
        PushColor();
        return;
    }
    ColorTimesIr();
    if (kind == LIGHT_NCC) {
        ShiftedColorFinish(sf, lm);
    } else {
        DepthCue(sf, lm);
    }
}

unsigned int GteReadData(int reg)
{
    switch (reg) {
    case R_VZ0:
    case R_VZ1:
    case R_VZ2:
    case R_IR0:
    case R_IR1:
    case R_IR2:
    case R_IR3:
        return (uint32_t)(int32_t)(int16_t)D(reg);
    case R_OTZ:
    case R_SZ0:
    case R_SZ1:
    case R_SZ2:
    case R_SZ3:
        return D(reg) & 0xFFFF;
    case R_SXYP:
        return D(R_SXY2);
    case R_IRGB:
    case R_ORGB: {
        uint32_t out = 0;
        int i;
        for (i = 0; i < 3; i++) {
            int32_t c = (int16_t)D(R_IR1 + i) >> 7;
            if (c < 0) {
                c = 0;
            } else if (c > 0x1F) {
                c = 0x1F;
            }
            out |= (uint32_t)c << (5 * i);
        }
        return out;
    }
    default:
        return D(reg);
    }
}

void GteWriteData(int reg, unsigned int value)
{
    switch (reg) {
    case R_SXYP:
        D(R_SXY0) = D(R_SXY1);
        D(R_SXY1) = D(R_SXY2);
        D(R_SXY2) = value;
        break;
    case R_IRGB:
        D(R_IR1) = (value & 0x1F) << 7;
        D(R_IR2) = ((value >> 5) & 0x1F) << 7;
        D(R_IR3) = ((value >> 10) & 0x1F) << 7;
        D(R_IRGB) = value & 0x7FFF;
        break;
    case R_ORGB:
        break;
    case R_LZCS: {
        uint32_t x = (value & 0x80000000u) ? ~value : value;
        uint32_t n = 0;
        while (n < 32 && !(x & (0x80000000u >> n))) {
            n++;
        }
        D(R_LZCS) = value;
        D(R_LZCR) = n;
        break;
    }
    case R_LZCR:
        break;
    default:
        D(reg) = value;
        break;
    }
}

void GteWriteCtrl(int reg, unsigned int value)
{
    switch (reg) {
    case C_RT4:
    case C_L4:
    case C_LR4:
    case C_H:
    case C_DQA:
    case C_ZSF3:
    case C_ZSF4:
        value = (uint32_t)(int32_t)(int16_t)value;
        break;
    case C_FLAG:
        value &= 0x7FFFF000u;
        if (value & 0x7F87E000u) {
            value |= 0x80000000u;
        }
        break;
    default:
        break;
    }
    C(reg) = value;
}

void GteExecute(unsigned int command)
{
    int sf = (command >> 19) & 1;
    int lm = (command >> 10) & 1;
    int shift = sf * 12;
    int i;

    sFlag = 0;
    switch (command & 0x3F) {
    case 0x01: /* RTPS */
        TransformPerspective(0, sf, lm, 1);
        break;
    case 0x30: /* RTPT */
        TransformPerspective(0, sf, lm, 0);
        TransformPerspective(1, sf, lm, 0);
        TransformPerspective(2, sf, lm, 1);
        break;
    case 0x06: { /* NCLIP */
        int64_t x0 = Lo(D(R_SXY0)), y0 = Hi(D(R_SXY0));
        int64_t x1 = Lo(D(R_SXY1)), y1 = Hi(D(R_SXY1));
        int64_t x2 = Lo(D(R_SXY2)), y2 = Hi(D(R_SXY2));
        SetMac0(x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1);
        break;
    }
    case 0x0C: { /* OP */
        int64_t d1 = MatrixElement(C_RT0, 0, 0), d2 = MatrixElement(C_RT0, 1, 1), d3 = MatrixElement(C_RT0, 2, 2);
        int64_t ir1 = (int16_t)D(R_IR1), ir2 = (int16_t)D(R_IR2), ir3 = (int16_t)D(R_IR3);
        SetMac(1, ir3 * d2 - ir2 * d3, shift);
        SetMac(2, ir1 * d3 - ir3 * d1, shift);
        SetMac(3, ir2 * d1 - ir1 * d2, shift);
        MacToIr(lm, sf, (ir2 * d1 - ir1 * d2));
        break;
    }
    case 0x10: { /* DPCS */
        uint32_t rgbc = D(R_RGBC);
        SetMac(1, (int64_t)(rgbc & 0xFF) << 16, 0);
        SetMac(2, (int64_t)((rgbc >> 8) & 0xFF) << 16, 0);
        SetMac(3, (int64_t)((rgbc >> 16) & 0xFF) << 16, 0);
        DepthCue(sf, lm);
        break;
    }
    case 0x2A: /* DPCT */
        for (i = 0; i < 3; i++) {
            uint32_t rgb = D(R_RGB0);
            SetMac(1, (int64_t)(rgb & 0xFF) << 16, 0);
            SetMac(2, (int64_t)((rgb >> 8) & 0xFF) << 16, 0);
            SetMac(3, (int64_t)((rgb >> 16) & 0xFF) << 16, 0);
            DepthCue(sf, lm);
        }
        break;
    case 0x11: /* INTPL */
        SetMac(1, (int64_t)(int16_t)D(R_IR1) << 12, 0);
        SetMac(2, (int64_t)(int16_t)D(R_IR2) << 12, 0);
        SetMac(3, (int64_t)(int16_t)D(R_IR3) << 12, 0);
        DepthCue(sf, lm);
        break;
    case 0x29: /* DCPL */
        ColorTimesIr();
        DepthCue(sf, lm);
        break;
    case 0x12: /* MVMVA */
        MultiplyMatrixVector((command >> 17) & 3, (command >> 15) & 3, (command >> 13) & 3, sf, lm);
        break;
    case 0x13: /* NCDS */
        NormalColorVertex(0, LIGHT_NCD, sf, lm);
        break;
    case 0x16: /* NCDT */
        for (i = 0; i < 3; i++) {
            NormalColorVertex(i, LIGHT_NCD, sf, lm);
        }
        break;
    case 0x1B: /* NCCS */
        NormalColorVertex(0, LIGHT_NCC, sf, lm);
        break;
    case 0x3F: /* NCCT */
        for (i = 0; i < 3; i++) {
            NormalColorVertex(i, LIGHT_NCC, sf, lm);
        }
        break;
    case 0x1E: /* NCS */
        NormalColorVertex(0, LIGHT_NC, sf, lm);
        break;
    case 0x20: /* NCT */
        for (i = 0; i < 3; i++) {
            NormalColorVertex(i, LIGHT_NC, sf, lm);
        }
        break;
    case 0x14: /* CDP */
        MultiplyMatrixVector(2, 3, 1, sf, lm);
        ColorTimesIr();
        DepthCue(sf, lm);
        break;
    case 0x1C: /* CC */
        MultiplyMatrixVector(2, 3, 1, sf, lm);
        ColorTimesIr();
        ShiftedColorFinish(sf, lm);
        break;
    case 0x28: /* SQR */
        for (i = 0; i < 3; i++) {
            int64_t ir = (int16_t)D(R_IR1 + i);
            SetMac(i + 1, ir * ir, shift);
        }
        MacToIr(lm, sf, (int64_t)(int32_t)D(R_MAC3) << shift);
        break;
    case 0x2D: { /* AVSZ3 */
        int64_t sum = (int64_t)(D(R_SZ1) & 0xFFFF) + (D(R_SZ2) & 0xFFFF) + (D(R_SZ3) & 0xFFFF);
        int64_t mac0 = (int64_t)(int16_t)C(C_ZSF3) * sum;
        int32_t otz;
        SetMac0(mac0);
        otz = (int32_t)(mac0 >> 12);
        if (otz < 0) {
            otz = 0;
            sFlag |= FLAG_SZ;
        } else if (otz > 0xFFFF) {
            otz = 0xFFFF;
            sFlag |= FLAG_SZ;
        }
        D(R_OTZ) = (uint32_t)otz;
        break;
    }
    case 0x2E: { /* AVSZ4 */
        int64_t sum = (int64_t)(D(R_SZ0) & 0xFFFF) + (D(R_SZ1) & 0xFFFF) + (D(R_SZ2) & 0xFFFF) +
                      (D(R_SZ3) & 0xFFFF);
        int64_t mac0 = (int64_t)(int16_t)C(C_ZSF4) * sum;
        int32_t otz;
        SetMac0(mac0);
        otz = (int32_t)(mac0 >> 12);
        if (otz < 0) {
            otz = 0;
            sFlag |= FLAG_SZ;
        } else if (otz > 0xFFFF) {
            otz = 0xFFFF;
            sFlag |= FLAG_SZ;
        }
        D(R_OTZ) = (uint32_t)otz;
        break;
    }
    case 0x3D: /* GPF */
        for (i = 0; i < 3; i++) {
            SetMac(i + 1, (int64_t)(int16_t)D(R_IR0) * (int16_t)D(R_IR1 + i), shift);
        }
        MacToIr(lm, sf, (int64_t)(int32_t)D(R_MAC3) << shift);
        PushColor();
        break;
    case 0x3E: /* GPL */
        for (i = 0; i < 3; i++) {
            int64_t acc = ((int64_t)(int32_t)D(R_MAC1 + i) << shift) +
                          (int64_t)(int16_t)D(R_IR0) * (int16_t)D(R_IR1 + i);
            SetMac(i + 1, acc, shift);
        }
        MacToIr(lm, sf, (int64_t)(int32_t)D(R_MAC3) << shift);
        PushColor();
        break;
    default:
        break;
    }
    if (sFlag & 0x7F87E000u) {
        sFlag |= 0x80000000u;
    }
    C(C_FLAG) = sFlag;
}
