/* The MDEC, the console's movie decoder, behind its registers (mmio.c), for the decompiled LIBPRESS.
 *
 * LIBPRESS writes a command to the data register and sends what follows it by DMA channel 0; channel 1 takes
 * the decoded picture. Tables load at once; a decode command keeps its run-length data where it lies in main
 * RAM, and each output request decodes the macroblocks it asks for straight into its destination, then raises
 * channel 1's interrupt (the movie player's callback). The decoding follows the hardware as psx-spx documents
 * it: run-length blocks (Cr, Cb, four Y), dequantised in zigzag order, an 8x8 inverse DCT by the table the
 * library loads, then YUV to 15- or 24-bit RGB. Integer arithmetic throughout, so every machine decodes alike. */

#include <stdint.h>
#include <string.h>
#include "psxport.h"
#include "mmio.h"
#include "mdec.h"
#include "kernel_port.h"
#include "host.h"

#define MDEC_DATA    0x1F801820u
#define MDEC_CONTROL 0x1F801824u
#define DMA0_MADR    0x1F801080u
#define DMA0_BCR     0x1F801084u
#define DMA0_CHCR    0x1F801088u
#define DMA1_MADR    0x1F801090u
#define DMA1_BCR     0x1F801094u
#define DMA1_CHCR    0x1F801098u
#define CHCR_BUSY    0x01000000u
#define STATUS_IDLE  0x80040000u /* output FIFO empty, nothing busy */

static const uint8_t sZigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

static struct {
    uint32_t command;     /* the last command word written */
    uint8_t quantY[64];   /* zigzag order */
    uint8_t quantC[64];
    int16_t idct[64];     /* [u * 8 + x] */
    uint32_t input;       /* the decode command's run-length data: address and halfwords left */
    uint32_t inputLeft;
    uint32_t pending[192]; /* words of a macroblock decoded past the last request */
    int pendingCount, pendingRead;
} sMdec;

static void *Memory(uint32_t address)
{
    return (void *)(uintptr_t)(address < PSX_RAM_SIZE ? (PSX_RAM_BASE | address) : address);
}

static uint16_t NextHalf(void)
{
    uint16_t v;
    if (sMdec.inputLeft == 0) {
        return 0xFE00;
    }
    v = *(uint16_t *)Memory(sMdec.input);
    sMdec.input += 2;
    sMdec.inputLeft--;
    return v;
}

static int Signed10(uint16_t v)
{
    return (int)((int16_t)(v << 6) >> 6);
}

static int Clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* One block: run-length codes to coefficients, then the inverse DCT; out is the spatial block. */
static void DecodeBlock(const uint8_t *quant, int out[64])
{
    int coef[64];
    int64_t rows[64];
    uint16_t n;
    int k = 0, scale, value, x, y, u;

    memset(coef, 0, sizeof(coef));
    do {
        n = NextHalf();
    } while (n == 0xFE00 && sMdec.inputLeft != 0);
    scale = (n >> 10) & 0x3F;
    value = Signed10(n) * quant[0];
    for (;;) {
        if (scale == 0) {
            value = Signed10(n) * 2;
        }
        value = Clamp(value, -0x400, 0x3FF);
        coef[scale > 0 ? sZigzag[k] : k] = value;
        n = NextHalf();
        k += ((n >> 10) & 0x3F) + 1;
        if (k > 63) {
            break;
        }
        value = (Signed10(n) * quant[k] * scale + 4) / 8;
    }
    /* Rows, then columns: out[y][x] = sum over v, u of coef[v][u] * T[u][x] * T[v][y] / 2^32. */
    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) {
            int64_t sum = 0;
            for (u = 0; u < 8; u++) {
                sum += (int64_t)coef[y * 8 + u] * sMdec.idct[u * 8 + x];
            }
            rows[y * 8 + x] = sum;
        }
    }
    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) {
            int64_t sum = 0;
            for (u = 0; u < 8; u++) {
                sum += rows[u * 8 + x] * sMdec.idct[u * 8 + y];
            }
            out[y * 8 + x] = (int)((sum + ((int64_t)1 << 31)) >> 32);
        }
    }
}

/* A 16x16 macroblock in the command's depth: 128 words at 15 bits, 192 at 24. Returns its words. */
static int DecodeMacroblock(uint32_t *out)
{
    int cr[64], cb[64], lum[4][64];
    int depth = (sMdec.command >> 27) & 3, isSigned = (sMdec.command >> 26) & 1;
    uint32_t mask = (sMdec.command >> 25) & 1 ? 0x8000u : 0;
    uint8_t *bytes = (uint8_t *)out;
    uint16_t *halves = (uint16_t *)out;
    int b, x, y;

    DecodeBlock(sMdec.quantC, cr);
    DecodeBlock(sMdec.quantC, cb);
    for (b = 0; b < 4; b++) {
        DecodeBlock(sMdec.quantY, lum[b]);
    }
    for (y = 0; y < 16; y++) {
        for (x = 0; x < 16; x++) {
            int l = lum[(y >> 3) * 2 + (x >> 3)][(y & 7) * 8 + (x & 7)];
            int r_ = cr[(y >> 1) * 8 + (x >> 1)], b_ = cb[(y >> 1) * 8 + (x >> 1)];
            int r = Clamp(l + ((1436 * r_) >> 10), -128, 127);
            int g = Clamp(l + ((-352 * b_ - 731 * r_) >> 10), -128, 127);
            int bl = Clamp(l + ((1815 * b_) >> 10), -128, 127);
            if (!isSigned) {
                r += 128;
                g += 128;
                bl += 128;
            } else {
                r &= 0xFF;
                g &= 0xFF;
                bl &= 0xFF;
            }
            if (depth == 3) {
                halves[y * 16 + x] = (uint16_t)((r >> 3) | (g >> 3) << 5 | (bl >> 3) << 10 | mask);
            } else {
                uint8_t *p = &bytes[(y * 16 + x) * 3];
                p[0] = (uint8_t)r;
                p[1] = (uint8_t)g;
                p[2] = (uint8_t)bl;
            }
        }
    }
    return depth == 3 ? 128 : 192;
}

/* Channel 0: the words after the command. */
static void TakeInput(uint32_t madr, uint32_t words)
{
    uint32_t command = sMdec.command;
    switch (command >> 29) {
    case 1: /* decode: the run-length data stays where it is */
        sMdec.input = madr;
        sMdec.inputLeft = words * 2;
        sMdec.pendingCount = sMdec.pendingRead = 0;
        break;
    case 2: /* quantisation tables: luminance, and chrominance when bit 0 is set */
        memcpy(sMdec.quantY, Memory(madr), 64);
        memcpy(sMdec.quantC, (command & 1) ? (uint8_t *)Memory(madr) + 64 : sMdec.quantY, 64);
        break;
    case 3: /* the inverse DCT's table */
        memcpy(sMdec.idct, Memory(madr), sizeof(sMdec.idct));
        break;
    default:
        break;
    }
}

/* Channel 1: `words` of decoded picture to `madr`. */
static void GiveOutput(uint32_t madr, uint32_t words)
{
    uint32_t *out = (uint32_t *)Memory(madr);
    while (words > 0) {
        if (sMdec.pendingRead < sMdec.pendingCount) {
            *out++ = sMdec.pending[sMdec.pendingRead++];
            words--;
            continue;
        }
        sMdec.pendingCount = DecodeMacroblock(sMdec.pending);
        sMdec.pendingRead = 0;
    }
}

void MdecCheckTables(void)
{
    /* vlc.s reads the two DC tables at CF_DVLC - 2048 and - 1024 (table.c keeps them in that order). */
    extern unsigned short DCL_DVLC[256][2], DCC_DVLC[256][2];
    extern unsigned long CF_DVLC[8192][2];
    if ((char *)CF_DVLC - (char *)DCL_DVLC != 2048 || (char *)CF_DVLC - (char *)DCC_DVLC != 1024) {
        HostFatal("LIBPRESS's VLC tables are not laid out as its decoder reads them");
    }
}

static uint32_t Words(uint32_t bcr)
{
    return (bcr & 0xFFFF) * (bcr >> 16 ? bcr >> 16 : 1);
}

void MdecRegisterWritten(uint32_t address, uint32_t value)
{
    if (address == MDEC_DATA) {
        sMdec.command = value;
    } else if (address == MDEC_CONTROL && (value & 0x80000000u)) {
        sMdec.inputLeft = 0;
        sMdec.pendingCount = sMdec.pendingRead = 0;
    }
    /* Reads see an idle decoder: work is done the moment it is asked for. */
    *(uint32_t *)MmioRegister(MDEC_CONTROL) = STATUS_IDLE;
    *(uint32_t *)MmioRegister(MDEC_DATA) = 0;
}

void MdecDmaWritten(uint32_t address)
{
    uint32_t *chcr;
    if (address == DMA0_CHCR) {
        chcr = (uint32_t *)MmioRegister(DMA0_CHCR);
        if (*chcr & CHCR_BUSY) {
            TakeInput(*(uint32_t *)MmioRegister(DMA0_MADR), Words(*(uint32_t *)MmioRegister(DMA0_BCR)));
            *chcr &= ~CHCR_BUSY;
            KernelRaiseDma(0);
        }
    } else if (address == DMA1_CHCR) {
        chcr = (uint32_t *)MmioRegister(DMA1_CHCR);
        if (*chcr & CHCR_BUSY) {
            GiveOutput(*(uint32_t *)MmioRegister(DMA1_MADR), Words(*(uint32_t *)MmioRegister(DMA1_BCR)));
            *chcr &= ~CHCR_BUSY;
            KernelRaiseDma(1);
        }
    }
}
