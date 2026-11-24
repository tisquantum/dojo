/* What the port's kernel layer (kernel.c) offers the emulated hardware. */

#ifndef PORT_KERNEL_PORT_H
#define PORT_KERNEL_PORT_H

/* Raises a DMA channel's completion interrupt; its callback runs at the game's next kernel call. */
void KernelRaiseDma(int channel);
/* Runs the interrupts raised since the last call. */
void KernelPollInterrupts(void);
/* For a library call the game polls in a loop with no other way out (a stream's next frame): the time one
 * poll takes on the console, `lines` of it, passes, and the interrupts raised meanwhile run. */
void KernelPollWait(unsigned long lines);
/* For the host, while the game is suspended: whether it yielded at a vertical blank (else at the end of a
 * tick), and emulated time in lines and fields. */
int KernelLastYieldWasBlank(void);
unsigned long KernelLine(void);
unsigned long KernelFieldCount(void);

#endif
