/* Two execution contexts on one thread: the host's and the game's, which runs on a stack inside the
 * executable image. The game's stack must lie there: it links its local GPU packets into ordering tables,
 * whose links hold only 24 bits of address. */

#ifndef PORT_CONTEXT_H
#define PORT_CONTEXT_H

/* Starts the game's context at `entry` on its own stack; the first ContextSwitchToGame runs it. */
void ContextInit(void (*entry)(void));
void ContextSwitchToGame(void);
/* For save states, while the game is suspended: its stack's bounds and its saved stack pointer. */
void ContextGameStack(void **low, void **high);
void *ContextGameStackPointer(void);
void ContextSetGameStackPointer(void *sp);
void ContextSwitchToHost(void);

#endif
