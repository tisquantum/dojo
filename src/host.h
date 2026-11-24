/* The host side of the port: the window, input, pacing, and the context the game runs in. */

#ifndef PORT_HOST_H
#define PORT_HOST_H

/* Called by the kernel layer at each emulated vertical blank: switches to the host until the next field. */
void HostVerticalBlank(void);
/* Writes the controllers into the game's receive buffers, before each tick: a press counts from the next. */
void HostFillPads(void);
/* This machine's controller as the game should see it now (netplay.c samples it before each tick). */
unsigned HostLocalButtons(void);
void HostWritePadPort(int port, int connected, unsigned buttons, const unsigned char analog[4]);
void HostSetRumbleBuffers(unsigned char *bufA, long lenA, unsigned char *bufB, long lenB);
/* Queues interleaved stereo samples at 44.1 kHz for the audio output. */
void HostQueueAudio(const short *stereo, int frames);
/* Shows a tick that ends inside a field and waits for its time (netplay.c runs fields of several). */
void HostPresentTick(void);
/* The path of the player's disc image. */
const char *HostDiscPath(void);
void HostFatal(const char *fmt, ...);

/* For online play (online.c). The simulation as it was before the game first ran: power-on, the same on
 * every machine. */
const unsigned char *HostBootState(void);
/* After a state is loaded outside a rollback: real time follows emulated time from here. */
void HostResetPacing(void);
/* The widescreen setting of the simulation (psxport.h), and this machine's own; the simulation's rate. */
void HostSetSimulationMargin(int margin);
int HostOwnMargin(void);
int HostOwnTicksPerFrame(void);

#endif
