/* The port's sound processor (spu.h), written from the documented behaviour of the console's SPU.
 *
 * The registers live in the I/O page (mmio.c): the game reads them there, and each write reaches
 * SpuWriteRegister, which acts on it at once (key on and off, transfers) and keeps the read-only registers
 * (status, envelope levels, end flags) current. Samples are produced in emulated time, a field's worth at
 * each vertical blank, so sound follows the game exactly whatever the host does. */

#include <string.h>
#include "host.h"
#include "kernel_port.h"
#include "mmio.h"
#include "psxport.h"
#include "spu.h"

#define SPU_RAM_SIZE 0x80000
#define VOICE_COUNT  24
#define NTSC_FIELD_RATE_X1000 59940

/* Register halfword indices (from 0x1F801C00). */
#define R_MVOL_L   0xC0
#define R_MVOL_R   0xC1
#define R_RVOL_L   0xC2
#define R_RVOL_R   0xC3
#define R_KON_L    0xC4
#define R_KON_H    0xC5
#define R_KOFF_L   0xC6
#define R_KOFF_H   0xC7
#define R_PMON_L   0xC8
#define R_PMON_H   0xC9
#define R_NON_L    0xCA
#define R_NON_H    0xCB
#define R_EON_L    0xCC
#define R_EON_H    0xCD
#define R_ENDX_L   0xCE
#define R_ENDX_H   0xCF
#define R_MBASE    0xD1
#define R_IRQ_ADDR 0xD2
#define R_TSA      0xD3
#define R_DATA     0xD4
#define R_SPUCNT   0xD5
#define R_TCTRL    0xD6
#define R_SPUSTAT  0xD7
#define R_CDVOL_L  0xD8
#define R_CDVOL_R  0xD9
#define R_REVERB   0xE0 /* 32 reverb configuration registers */

enum { ENV_OFF, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

typedef struct Voice {
    uint32_t current;     /* address of the block being played */
    uint32_t repeat;      /* the loop address */
    uint32_t counter;     /* pitch counter: sample index in 12.12 within the block */
    int16_t decoded[28];  /* the current block's samples */
    int16_t history[2];   /* the ADPCM filter's previous outputs */
    int16_t last[4];      /* the last samples played, for interpolation */
    int envPhase;
    int32_t envLevel;
    int envCounter;
    int blockLoaded;
    int16_t output;       /* the last output, for the next voice's pitch modulation */
} Voice;

static uint8_t sRam[SPU_RAM_SIZE];
static Voice sVoices[VOICE_COUNT];
/* Where this process mapped the registers: the machine's, not the simulation's. */
#pragma clang section bss = ".bss$render" data = ".data$render"
static uint16_t *sRegs;
#pragma clang section bss = ".bss$sim" data = ".data$sim"
static uint32_t sTransferAddress;
static uint32_t sReverbCurrent;
static int32_t sReverbInL, sReverbInR, sReverbOutL, sReverbOutR;
static int sReverbPhase;
static uint32_t sNoiseLevel = 1;
static int sNoiseCounter;
static int sSampleRemainder;

/* CD audio: a ring of stereo frames at 44.1 kHz, resampled from what XA decoding queues. */
#define CD_RING 16384
static int16_t sCdRing[CD_RING][2];
static unsigned sCdRead, sCdWrite;
static uint32_t sCdResamplePos;
static int16_t sCdPrev[2];

static const int sAdpcmPos[5] = {0, 60, 115, 98, 122};
static const int sAdpcmNeg[5] = {0, 0, -52, -55, -60};

static int16_t Clamp16(int32_t v)
{
    return (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
}

static uint16_t *VoiceReg(int voice, int reg)
{
    return &sRegs[voice * 8 + reg];
}

static void DecodeBlock(Voice *v)
{
    const uint8_t *block = &sRam[v->current & (SPU_RAM_SIZE - 1)];
    int shift = block[0] & 0x0F;
    int filter = (block[0] >> 4) & 7;
    int i;
    if (shift > 12) {
        shift = 9;
    }
    if (filter > 4) {
        filter = 4;
    }
    for (i = 0; i < 28; i++) {
        int nibble = (block[2 + i / 2] >> ((i & 1) * 4)) & 0x0F;
        int32_t sample = (int32_t)(int16_t)(nibble << 12) >> shift;
        sample += (v->history[0] * sAdpcmPos[filter] + v->history[1] * sAdpcmNeg[filter] + 32) >> 6;
        v->decoded[i] = Clamp16(sample);
        v->history[1] = v->history[0];
        v->history[0] = v->decoded[i];
    }
    if (block[1] & 4) {
        v->repeat = v->current; /* loop start */
    }
    v->blockLoaded = 1;
}

/* One envelope step for a rate of the documented form (0..127), increasing or decreasing. */
static void EnvelopeStep(Voice *v, int rate, int decreasing, int exponential)
{
    int shift = rate >> 2;
    int32_t step = decreasing ? -8 + (rate & 3) : 7 - (rate & 3);
    int cycles = 1 << (shift > 11 ? shift - 11 : 0);
    if (shift < 11) {
        step <<= 11 - shift;
    }
    if (exponential && !decreasing && v->envLevel > 0x6000) {
        cycles *= 4;
    }
    if (exponential && decreasing) {
        step = step * v->envLevel >> 15;
    }
    if (++v->envCounter < cycles) {
        return;
    }
    v->envCounter = 0;
    v->envLevel += step;
    if (v->envLevel > 0x7FFF) {
        v->envLevel = 0x7FFF;
    }
    if (v->envLevel < 0) {
        v->envLevel = 0;
    }
}

static void RunEnvelope(Voice *v, int index)
{
    uint16_t adsr1 = *VoiceReg(index, 4), adsr2 = *VoiceReg(index, 5);
    switch (v->envPhase) {
    case ENV_ATTACK:
        EnvelopeStep(v, (adsr1 >> 8) & 0x7F, 0, adsr1 >> 15);
        if (v->envLevel >= 0x7FFF) {
            v->envPhase = ENV_DECAY;
            v->envCounter = 0;
        }
        break;
    case ENV_DECAY: {
        int32_t sustain = ((adsr1 & 0x0F) + 1) * 0x800;
        EnvelopeStep(v, ((adsr1 >> 4) & 0x0F) * 4, 1, 1);
        if (v->envLevel <= sustain) {
            v->envPhase = ENV_SUSTAIN;
            v->envCounter = 0;
        }
        break;
    }
    case ENV_SUSTAIN:
        EnvelopeStep(v, (adsr2 >> 6) & 0x7F, (adsr2 >> 14) & 1, adsr2 >> 15);
        break;
    case ENV_RELEASE:
        EnvelopeStep(v, (adsr2 & 0x1F) * 4, 1, (adsr2 >> 5) & 1);
        if (v->envLevel == 0) {
            v->envPhase = ENV_OFF;
        }
        break;
    default:
        break;
    }
}

static void KeyOn(int index)
{
    Voice *v = &sVoices[index];
    v->current = (uint32_t)*VoiceReg(index, 3) * 8;
    v->repeat = v->current;
    v->counter = 0;
    v->history[0] = v->history[1] = 0;
    memset(v->last, 0, sizeof(v->last));
    v->envPhase = ENV_ATTACK;
    v->envLevel = 0;
    v->envCounter = 0;
    v->blockLoaded = 0;
    if (index < 16) {
        sRegs[R_ENDX_L] &= ~(1u << index);
    } else {
        sRegs[R_ENDX_H] &= ~(1u << (index - 16));
    }
}

static void KeyOff(int index)
{
    if (sVoices[index].envPhase != ENV_OFF) {
        sVoices[index].envPhase = ENV_RELEASE;
        sVoices[index].envCounter = 0;
    }
}

static int VoiceBit(int lowReg, int index)
{
    return index < 16 ? (sRegs[lowReg] >> index) & 1 : (sRegs[lowReg + 1] >> (index - 16)) & 1;
}

/* A voice's volume register as a level: fixed volumes only, as the game's libraries set them. */
static int32_t VolumeLevel(uint16_t reg)
{
    if (reg & 0x8000) {
        return 0x3FFF; /* a sweep: held at its full level */
    }
    return (int16_t)(reg << 1) >> 1;
}

/* Produces voice `index`'s next sample, advancing its block and envelope. */
static int32_t VoiceSample(int index, int32_t modulator)
{
    Voice *v = &sVoices[index];
    int32_t sample, step;
    int position, frac;

    if (v->envPhase == ENV_OFF) {
        v->output = 0;
        return 0;
    }
    if (!v->blockLoaded) {
        DecodeBlock(v);
    }
    step = *VoiceReg(index, 2);
    if (step > 0x4000) {
        step = 0x4000;
    }
    if (index > 0 && VoiceBit(R_PMON_L, index)) {
        step = (step * (modulator + 0x8000)) >> 15;
    }
    position = v->counter >> 12;
    frac = v->counter & 0xFFF;
    if (VoiceBit(R_NON_L, index)) {
        sample = (int16_t)sNoiseLevel;
    } else {
        int32_t a = v->decoded[position];
        int32_t b = position + 1 < 28 ? v->decoded[position + 1] : a;
        sample = a + (((b - a) * frac) >> 12);
    }
    v->counter += (uint32_t)step;
    while ((v->counter >> 12) >= 28) {
        const uint8_t *block = &sRam[v->current & (SPU_RAM_SIZE - 1)];
        int flags = block[1];
        v->counter -= 28 << 12;
        if (flags & 1) {
            /* The block ends a loop: mark the voice's end and repeat, or stop if not repeating. */
            if (index < 16) {
                sRegs[R_ENDX_L] |= 1u << index;
            } else {
                sRegs[R_ENDX_H] |= 1u << (index - 16);
            }
            v->current = v->repeat;
            if (!(flags & 2)) {
                v->envPhase = ENV_OFF;
                v->envLevel = 0;
            }
        } else {
            v->current += 16;
        }
        DecodeBlock(v);
    }
    RunEnvelope(v, index);
    *VoiceReg(index, 6) = (uint16_t)v->envLevel;
    sample = (sample * v->envLevel) >> 15;
    v->output = (int16_t)sample;
    return sample;
}

/* The reverb unit's buffer, addressed relative to its moving current position. */
static int16_t *ReverbCell(uint32_t offset)
{
    uint32_t base = (uint32_t)sRegs[R_MBASE] * 8;
    uint32_t span = SPU_RAM_SIZE - base;
    uint32_t address;
    if (span == 0) {
        span = 2;
    }
    address = base + ((sReverbCurrent - base + offset) % span);
    return (int16_t *)&sRam[address & (SPU_RAM_SIZE - 2)];
}

#define RV(n) ((int32_t)(int16_t)sRegs[R_REVERB + (n)])
#define RA(n) ((uint32_t)sRegs[R_REVERB + (n)] * 8)

static int32_t Mul15(int32_t a, int32_t b)
{
    return (a * b) >> 15;
}

/* One step of the reverb at 22.05 kHz, from the inputs gathered over the last two samples. */
static void ReverbStep(int32_t inL, int32_t inR)
{
    int32_t vIIR = RV(2), vCOMB1 = RV(3), vCOMB2 = RV(4), vCOMB3 = RV(5), vCOMB4 = RV(6), vWALL = RV(7);
    int32_t vAPF1 = RV(8), vAPF2 = RV(9), vLIN = RV(30), vRIN = RV(31);
    uint32_t dAPF1 = RA(0), dAPF2 = RA(1);
    int32_t lIn = Mul15(inL, vLIN), rIn = Mul15(inR, vRIN);
    int32_t l, r;

    if (sRegs[R_SPUCNT] & 0x80) {
        int16_t *mLSAME = ReverbCell(RA(10)), *mRSAME = ReverbCell(RA(11));
        int16_t *mLDIFF = ReverbCell(RA(18)), *mRDIFF = ReverbCell(RA(19));
        *mLSAME = Clamp16(Mul15(lIn + Mul15(*ReverbCell(RA(16)), vWALL) - *ReverbCell(RA(10) - 2), vIIR) +
                          *ReverbCell(RA(10) - 2));
        *mRSAME = Clamp16(Mul15(rIn + Mul15(*ReverbCell(RA(17)), vWALL) - *ReverbCell(RA(11) - 2), vIIR) +
                          *ReverbCell(RA(11) - 2));
        *mLDIFF = Clamp16(Mul15(lIn + Mul15(*ReverbCell(RA(25)), vWALL) - *ReverbCell(RA(18) - 2), vIIR) +
                          *ReverbCell(RA(18) - 2));
        *mRDIFF = Clamp16(Mul15(rIn + Mul15(*ReverbCell(RA(24)), vWALL) - *ReverbCell(RA(19) - 2), vIIR) +
                          *ReverbCell(RA(19) - 2));
    }
    l = Mul15(vCOMB1, *ReverbCell(RA(12))) + Mul15(vCOMB2, *ReverbCell(RA(14))) +
        Mul15(vCOMB3, *ReverbCell(RA(20))) + Mul15(vCOMB4, *ReverbCell(RA(22)));
    r = Mul15(vCOMB1, *ReverbCell(RA(13))) + Mul15(vCOMB2, *ReverbCell(RA(15))) +
        Mul15(vCOMB3, *ReverbCell(RA(21))) + Mul15(vCOMB4, *ReverbCell(RA(23)));
    l = Clamp16(l - Mul15(vAPF1, *ReverbCell(RA(26) - dAPF1)));
    r = Clamp16(r - Mul15(vAPF1, *ReverbCell(RA(27) - dAPF1)));
    if (sRegs[R_SPUCNT] & 0x80) {
        *ReverbCell(RA(26)) = (int16_t)l;
        *ReverbCell(RA(27)) = (int16_t)r;
    }
    l = Clamp16(Mul15(l, vAPF1) + *ReverbCell(RA(26) - dAPF1));
    r = Clamp16(Mul15(r, vAPF1) + *ReverbCell(RA(27) - dAPF1));
    l = Clamp16(l - Mul15(vAPF2, *ReverbCell(RA(28) - dAPF2)));
    r = Clamp16(r - Mul15(vAPF2, *ReverbCell(RA(29) - dAPF2)));
    if (sRegs[R_SPUCNT] & 0x80) {
        *ReverbCell(RA(28)) = (int16_t)l;
        *ReverbCell(RA(29)) = (int16_t)r;
    }
    l = Clamp16(Mul15(l, vAPF2) + *ReverbCell(RA(28) - dAPF2));
    r = Clamp16(Mul15(r, vAPF2) + *ReverbCell(RA(29) - dAPF2));
    sReverbOutL = Mul15(l, (int16_t)sRegs[R_RVOL_L]);
    sReverbOutR = Mul15(r, (int16_t)sRegs[R_RVOL_R]);
    sReverbCurrent += 2;
    if (sReverbCurrent >= SPU_RAM_SIZE || sReverbCurrent < (uint32_t)sRegs[R_MBASE] * 8) {
        sReverbCurrent = (uint32_t)sRegs[R_MBASE] * 8;
    }
}

static void NextCdFrame(int32_t *l, int32_t *r)
{
    if (sCdRead != sCdWrite) {
        *l = sCdRing[sCdRead % CD_RING][0];
        *r = sCdRing[sCdRead % CD_RING][1];
        sCdRead++;
    } else {
        *l = *r = 0;
    }
}

/* The voices the game plays its sound effects on (include/voice_state.h): footsteps, swings, clashes and
 * hits, panned as heard. Not voices 0 and 1, its speech, nor 17 and up, the stage's ambience. */
#define HAPTIC_VOICE_FIRST 2
#define HAPTIC_VOICE_END   17

static void MixSample(int16_t out[2], int16_t haptic[2])
{
    int32_t left = 0, right = 0, revL = 0, revR = 0, cdL, cdR, hapticL = 0, hapticR = 0;
    int32_t modulator = 0;
    int i;

    haptic[0] = haptic[1] = 0;
    if (!(sRegs[R_SPUCNT] & 0x8000)) {
        out[0] = out[1] = 0;
        NextCdFrame(&cdL, &cdR);
        return;
    }
    /* The noise generator, clocked by SPUCNT's noise shift and step. */
    {
        int shift = (sRegs[R_SPUCNT] >> 10) & 0x0F, stepBits = (sRegs[R_SPUCNT] >> 8) & 3;
        sNoiseCounter += stepBits + 4;
        if (sNoiseCounter >= (0x20000 >> shift)) {
            uint32_t parity = ((sNoiseLevel >> 15) ^ (sNoiseLevel >> 12) ^ (sNoiseLevel >> 11) ^
                               (sNoiseLevel >> 10) ^ 1) & 1;
            sNoiseCounter -= 0x20000 >> shift;
            sNoiseLevel = (sNoiseLevel << 1) | parity;
        }
    }
    for (i = 0; i < VOICE_COUNT; i++) {
        int32_t s = VoiceSample(i, modulator);
        int32_t l = Mul15(s, VolumeLevel(*VoiceReg(i, 0))) * 2;
        int32_t r = Mul15(s, VolumeLevel(*VoiceReg(i, 1))) * 2;
        modulator = s;
        left += l;
        right += r;
        if (i >= HAPTIC_VOICE_FIRST && i < HAPTIC_VOICE_END) {
            hapticL += l;
            hapticR += r;
        }
        if (VoiceBit(R_EON_L, i)) {
            revL += l;
            revR += r;
        }
    }
    NextCdFrame(&cdL, &cdR);
    if (sRegs[R_SPUCNT] & 0x0001) {
        cdL = Mul15(cdL, (int16_t)sRegs[R_CDVOL_L]);
        cdR = Mul15(cdR, (int16_t)sRegs[R_CDVOL_R]);
        left += cdL;
        right += cdR;
        if (sRegs[R_SPUCNT] & 0x0004) {
            revL += cdL;
            revR += cdR;
        }
    } else {
        cdL = cdR = 0;
    }
    sReverbInL += revL;
    sReverbInR += revR;
    if (++sReverbPhase == 2) {
        ReverbStep(Clamp16(sReverbInL / 2), Clamp16(sReverbInR / 2));
        sReverbPhase = 0;
        sReverbInL = sReverbInR = 0;
    }
    left += sReverbOutL;
    right += sReverbOutR;
    if (!(sRegs[R_SPUCNT] & 0x4000)) {
        left = right = 0; /* muted */
        hapticL = hapticR = 0;
    }
    haptic[0] = Clamp16(hapticL); /* dry, and not at the player's volume: the haptics have their own */
    haptic[1] = Clamp16(hapticR);
    out[0] = Clamp16(Mul15(Clamp16(left), VolumeLevel(sRegs[R_MVOL_L])) * 2);
    out[1] = Clamp16(Mul15(Clamp16(right), VolumeLevel(sRegs[R_MVOL_R])) * 2);
}

static void UpdateStatus(void)
{
    /* SPUSTAT mirrors SPUCNT's low six bits; transfers finish at once, so it is never busy. */
    sRegs[R_SPUSTAT] = (uint16_t)(sRegs[R_SPUCNT] & 0x3F);
}

static void EnsureRegisters(void)
{
    if (!sRegs) {
        sRegs = (uint16_t *)MmioRegister(0x1F801C00);
    }
}

void SpuWriteRegister(uint32_t offset, uint16_t value)
{
    int index = (int)(offset >> 1);
    int i;

    EnsureRegisters();
    switch (index) {
    case R_KON_L:
    case R_KON_H:
        for (i = 0; i < 16; i++) {
            if (value & (1u << i)) {
                KeyOn(i + (index == R_KON_H ? 16 : 0));
            }
        }
        sRegs[index] = value;
        break;
    case R_KOFF_L:
    case R_KOFF_H:
        for (i = 0; i < 16; i++) {
            if (value & (1u << i)) {
                KeyOff(i + (index == R_KOFF_H ? 16 : 0));
            }
        }
        sRegs[index] = value;
        break;
    case R_ENDX_L:
    case R_ENDX_H:
    case R_SPUSTAT:
        break; /* read-only */
    case R_TSA:
        sRegs[index] = value;
        sTransferAddress = (uint32_t)value * 8;
        break;
    case R_DATA:
        *(uint16_t *)&sRam[sTransferAddress & (SPU_RAM_SIZE - 2)] = value;
        sTransferAddress = (sTransferAddress + 2) & (SPU_RAM_SIZE - 1);
        break;
    case R_SPUCNT:
        sRegs[index] = value;
        UpdateStatus();
        break;
    case R_MBASE:
        sRegs[index] = value;
        sReverbCurrent = (uint32_t)value * 8;
        break;
    default:
        if (index < VOICE_COUNT * 8 && (index & 7) == 6) {
            break; /* a voice's current envelope level is read-only */
        }
        sRegs[index] = value;
        break;
    }
}

void SpuDmaRegisterWritten(uint32_t address)
{
    uint32_t *chcr = (uint32_t *)MmioRegister(0x1F8010C8);
    uint32_t madr, bcr, words;
    uint8_t *mem;

    EnsureRegisters();
    if (address != 0x1F8010C8 || !(*chcr & 0x01000000)) {
        return;
    }
    madr = *(uint32_t *)MmioRegister(0x1F8010C0);
    bcr = *(uint32_t *)MmioRegister(0x1F8010C4);
    words = (bcr & 0xFFFF) * (bcr >> 16 ? bcr >> 16 : 1);
    /* The address the library gives is a pointer of the port's, or a physical address in main RAM. */
    mem = (uint8_t *)(uintptr_t)(madr < PSX_RAM_SIZE ? (PSX_RAM_BASE | madr) : madr);
    if (*chcr & 1) {
        uint32_t i;
        for (i = 0; i < words * 4; i++) {
            sRam[(sTransferAddress + i) & (SPU_RAM_SIZE - 1)] = mem[i];
        }
    } else {
        uint32_t i;
        for (i = 0; i < words * 4; i++) {
            mem[i] = sRam[(sTransferAddress + i) & (SPU_RAM_SIZE - 1)];
        }
    }
    sTransferAddress = (sTransferAddress + words * 4) & (SPU_RAM_SIZE - 1);
    *chcr &= ~0x01000000u;
    KernelRaiseDma(4);
}

void SpuQueueCdAudio(const int16_t *stereo, int frames, int rate)
{
    /* Resamples to 44.1 kHz by linear interpolation into the CD ring. */
    uint32_t step = (uint32_t)(((uint64_t)rate << 16) / SPU_SAMPLE_RATE);
    int i = 0;
    while (i < frames) {
        int32_t frac = (int32_t)(sCdResamplePos & 0xFFFF);
        int c;
        if (sCdWrite - sCdRead >= CD_RING) {
            sCdRead++; /* the ring is full: drop the oldest */
        }
        for (c = 0; c < 2; c++) {
            int32_t a = sCdPrev[c], b = stereo[i * 2 + c];
            sCdRing[sCdWrite % CD_RING][c] = (int16_t)(a + (((b - a) * frac) >> 16));
        }
        sCdWrite++;
        sCdResamplePos += step;
        while (sCdResamplePos >= 0x10000 && i < frames) {
            sCdResamplePos -= 0x10000;
            sCdPrev[0] = stereo[i * 2];
            sCdPrev[1] = stereo[i * 2 + 1];
            i++;
        }
    }
}

/* The field's samples, for the host to take once the game has yielded (SpuTakeFieldAudio): the host's audio
 * calls do not run on the game's stack. */
#pragma clang section bss = ".bss$render" data = ".data$render"
static int16_t sFieldAudio[1024][2];
static int16_t sFieldHaptics[1024][2];
static int sFieldAudioCount, sFieldHapticsCount;
#pragma clang section bss = ".bss$sim" data = ".data$sim"

const int16_t *SpuTakeFieldHaptics(int *frames)
{
    *frames = sFieldHapticsCount;
    sFieldHapticsCount = 0;
    return &sFieldHaptics[0][0];
}

const int16_t *SpuTakeFieldAudio(int *frames)
{
    *frames = sFieldAudioCount;
    sFieldAudioCount = 0;
    return &sFieldAudio[0][0];
}

void SpuVerticalBlank(void)
{
    int16_t (*buffer)[2] = sFieldAudio;
    int count;

    EnsureRegisters();
    /* 44100 samples a second over 59.94 fields: 735 or 736 a field. */
    sSampleRemainder += SPU_SAMPLE_RATE * 1000;
    count = sSampleRemainder / NTSC_FIELD_RATE_X1000;
    sSampleRemainder -= count * NTSC_FIELD_RATE_X1000;
    if (count > 1024) {
        count = 1024;
    }
    {
        int i;
        for (i = 0; i < count; i++) {
            MixSample(buffer[i], sFieldHaptics[i]);
        }
    }
    sFieldAudioCount = sFieldHapticsCount = count;
}
