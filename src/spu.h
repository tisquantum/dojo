/* The port's sound processor: the console SPU's 24 ADPCM voices, envelopes, reverb, sound RAM and the CD
 * audio input, behind its registers (mmio.c), mixed at 44.1 kHz to the host's audio output. */

#ifndef PORT_SPU_H
#define PORT_SPU_H

#include <stdint.h>

#define SPU_SAMPLE_RATE 44100

/* A write to the SPU register at byte offset `offset` from 0x1F801C00. */
void SpuWriteRegister(uint32_t offset, uint16_t value);
/* A write to one of DMA channel 4's registers (0x1F8010C0..CF), the SPU's. */
void SpuDmaRegisterWritten(uint32_t address);
/* Produces one field's samples, for the host to take with SpuTakeFieldAudio. */
void SpuVerticalBlank(void);
const int16_t *SpuTakeFieldAudio(int *frames);
/* Queues decoded CD audio (XA), interleaved stereo at `rate` Hz, for the SPU's CD input. */
void SpuQueueCdAudio(const int16_t *stereo, int frames, int rate);

#endif
