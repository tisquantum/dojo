/* XA audio: the ADPCM sound groups of the disc's interleaved audio sectors, decoded as the drive reads them
 * (libcd.c) and fed to the SPU's CD input through the CD mixing volumes the game sets (CdMix). */

#include <stdint.h>
#include <libcd.h>
#include "cdrom.h"
#include "spu.h"

#define SUBMODE_AUDIO 0x04
#define GROUPS        18
#define GROUP_SIZE    128

static const int sK0[4] = {0, 60, 115, 98};
static const int sK1[4] = {0, 0, -52, -55};

const CdlATV *CdAudioMix(void);
int CdXaFilter(int *file, int *channel);

static int16_t Clamp16(int32_t v)
{
    return (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
}

/* Decodes one 4-bit sound unit's 28 samples into out[stride * i]. */
static void DecodeUnit(const uint8_t *group, int unit, int16_t history[2], int16_t *out, int stride)
{
    uint8_t param = group[4 + unit];
    int shift = param & 0x0F, filter = (param >> 4) & 3;
    int i;
    if (shift > 12) {
        shift = 9;
    }
    for (i = 0; i < 28; i++) {
        uint8_t byte = group[16 + i * 4 + unit / 2];
        int nibble = (unit & 1) ? byte >> 4 : byte & 0x0F;
        int32_t sample = (int32_t)(int16_t)(nibble << 12) >> shift;
        sample += (history[0] * sK0[filter] + history[1] * sK1[filter] + 32) >> 6;
        history[1] = history[0];
        history[0] = Clamp16(sample);
        out[i * stride] = history[0];
    }
}

void CdXaSector(const unsigned char *raw)
{
    static int16_t historyL[2], historyR[2];
    static int16_t samples[GROUPS * 8 * 28 * 2];
    const unsigned char *sub = raw + 16;
    const unsigned char *data = raw + 24;
    int stereo, rate, frames = 0, file, channel, g, u;
    const CdlATV *mix;

    if (!(sub[2] & SUBMODE_AUDIO)) {
        return;
    }
    if (CdXaFilter(&file, &channel) && (sub[0] != file || sub[1] != channel)) {
        return;
    }
    if ((sub[3] >> 4) & 3) {
        return; /* 8-bit sound groups: the game's streams are all 4-bit */
    }
    stereo = (sub[3] & 3) == 1;
    rate = ((sub[3] >> 2) & 3) ? 18900 : 37800;
    for (g = 0; g < GROUPS; g++) {
        const uint8_t *group = data + g * GROUP_SIZE;
        for (u = 0; u < 8; u++) {
            if (stereo) {
                int16_t *out = &samples[(frames + (u / 2) * 28) * 2 + (u & 1)];
                DecodeUnit(group, u, (u & 1) ? historyR : historyL, out, 2);
            } else {
                int16_t mono[28];
                int i;
                DecodeUnit(group, u, historyL, mono, 1);
                for (i = 0; i < 28; i++) {
                    samples[(frames + u * 28 + i) * 2] = samples[(frames + u * 28 + i) * 2 + 1] = mono[i];
                }
            }
        }
        frames += stereo ? 4 * 28 : 8 * 28;
    }
    /* The CD mixing volumes, 0x80 for unity: CD left to SPU left and right, CD right to right and left. */
    mix = CdAudioMix();
    {
        int i;
        for (i = 0; i < frames; i++) {
            int32_t l = samples[i * 2], r = samples[i * 2 + 1];
            samples[i * 2] = Clamp16((l * mix->val0 + r * mix->val3) >> 7);
            samples[i * 2 + 1] = Clamp16((r * mix->val2 + l * mix->val1) >> 7);
        }
    }
    SpuQueueCdAudio(samples, frames, rate);
}
