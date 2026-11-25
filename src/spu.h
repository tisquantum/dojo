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
/* The same field's sound effects alone, dry (no music, speech or ambience), for a DualSense's haptics. */
const int16_t *SpuTakeFieldHaptics(int *frames);
/* Sound after a rollback (netplay.c). Each field run tells which it is and whether it is heard (a field run
 * again after a rollback isn't); the sounds each started are logged outside the simulation. After the fields
 * a rollback ran again, the sounds they started that were never heard play now from their beginning (up to
 * 8 fields late), on extra voices that touch nothing of the simulation, the game's own voice silent meanwhile. */
void SpuBeginField(long field, int heard);
void SpuEndFields(void);
void SpuRestoreAfterRollback(long from, long to);
unsigned SpuRestoredSounds(void);
/* Queues decoded CD audio (XA), interleaved stereo at `rate` Hz, for the SPU's CD input. */
void SpuQueueCdAudio(const int16_t *stereo, int frames, int rate);

#endif
