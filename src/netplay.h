/* Online play with rollback: two machines run the same simulation, each predicting the other's input and
 * correcting itself when the real input arrives. */

#ifndef PORT_NETPLAY_H
#define PORT_NETPLAY_H

#include <stdint.h>

/* Starts a session from DOJO_NET ("host:PORT" or "join:ADDRESS:PORT"); returns 0 when there is none.
 * Blocks until the other machine answers. `simMargin` is this machine's widescreen setting (psxport.h) and
 * `ticksPerFrame` its simulation rate (pc_substep.h); both use the wider and the faster. */
int NetplayStart(int simMargin, int ticksPerFrame, int *agreedMargin, int *agreedTicks);
int NetplayActive(void);

/* How a session started by online play (online.c) reaches the other machine: sends a packet to it, and
 * takes the next packet of the session's that has come (0 when none). */
typedef struct NetplayTransport {
    void (*send)(const void *data, int size);
    int (*receive)(void *buffer, int capacity);
} NetplayTransport;

/* Starts a session from the state the game is in, which both machines must share, field 0 from here;
 * `local` is this machine's controller port. */
void NetplayBegin(const NetplayTransport *transport, int local, int delay);
void NetplayEnd(void);
int NetplayLocalPlayer(void);
/* How long the other machine has been silent, in ms. */
uint32_t NetplaySilentMs(void);
/* The game state and bout winner (gMatchWinnerSide: -1, or the winning side) after the newest field both
 * machines have every input for; returns 0 before there is one. */
int NetplaySettled(int *gameState, int *winnerSide);
/* A packet of the rollback protocol's own, so the transport can tell them from its other traffic. */
int NetplayIsSessionPacket(const void *data, int size);
/* Memory cards are absent in an online session, and while online play prepares the state it starts from. */
int NetplayCardsOff(void);
/* For spectators (online.c): how many fields from the session's start both machines have every input for,
 * and the two controllers' inputs of one of them while the ring still holds it (returns 0 when not). */
uint32_t NetplaySettledFields(void);
int NetplayInputsAt(uint32_t field, uint16_t *port0, uint16_t *port1);
void NetplaySetCardsOff(int off);
/* Runs one field of the session: sends and receives inputs, rolls back if a prediction was wrong, and
 * advances the game by a field (each of its ticks shown through HostPresentTick) unless it is too far
 * ahead of the other machine. `localButtons` is this machine's player's controller. Returns 1 if the game
 * ran a field. */
int NetplayRunField(unsigned localButtons);
/* A line describing the session's state for the window title, or NULL. */
const char *NetplayStatus(void);

#endif
