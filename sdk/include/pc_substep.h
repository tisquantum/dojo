/* Simulation substeps (port/src/substep.c): frame rates above the console's 30, by the game's own code.
 *
 * Each of the console's frames is run as g_pcTicksPerFrame ticks, g_pcSubtick counting them from 0; the last
 * is the console's frame. A game state that substeps runs every tick, advancing continuous motion by its
 * share of the frame (PC_SHARE, PC_DECAY) and leaving discrete logic (counters, hits, events, decisions) to
 * the last tick (PC_LAST_SUBTICK); any other state runs on the last tick alone, as on the console. With one
 * tick a frame every share is the whole and every tick the last: the console's game exactly. */

#ifndef PC_SUBSTEP_H
#define PC_SUBSTEP_H

extern int g_pcTicksPerFrame; /* the setting: the same on both machines of an online match */
/* The frame being run: its ticks (g_pcTicksPerFrame if its state substeps, else 1) and the current one.
 * They are simulation state (a saved game resumes mid-frame) and set by the main loop (src/main.c). */
extern int g_pcFrameTicks;
extern int g_pcSubtick;

#define PC_LAST_SUBTICK (g_pcSubtick == g_pcFrameTicks - 1)
#define PC_FIRST_SUBTICK (g_pcSubtick == 0)
#define PC_TICKS_PER_FRAME g_pcFrameTicks
/* A per-frame counter's ++x: the next value on every tick, stored by the last. */
#define PC_FRAME_INC(x) (PC_LAST_SUBTICK ? ++(x) : (x) + 1)
/* A frame count as ticks, for what moves with it (a scene's camera sweep): `frames` is the count on the
 * frame's last tick, where this is frames * g_pcFrameTicks; the ticks before step up to it from the last
 * frame's. With one tick a frame it is `frames`. */
#define PC_TICKS(frames) ((frames) * g_pcFrameTicks - (g_pcFrameTicks - 1 - g_pcSubtick))
/* How far a tick is through its frame, in Q12: 4096 on the last tick. For what the game keeps only per frame
 * (a baked camera track, a recorded pose): a tick shows the last frame's value moving to this one's, the
 * last tick this one's exactly. PC_BLEND_ANGLE goes the short way round a 4096-unit turn. */
#define PC_FRAME_Q12 (((g_pcSubtick + 1) << 12) / g_pcFrameTicks)
#define PC_BLEND(last, now)                                                                                    \
    (PC_LAST_SUBTICK ? (now) : (last) + (int)(((long long)((now) - (last)) * PC_FRAME_Q12) >> 12))
#define PC_BLEND_ANGLE(last, now)                                                                              \
    (PC_LAST_SUBTICK ? (now)                                                                                   \
                     : (last) + (int)(((long long)((((now) - (last) + 2048) & 4095) - 2048) * PC_FRAME_Q12) >> 12))
/* A per-frame amount that comes all at once at the end of the frame (a velocity's gravity): 0 before. */
#define PC_ON_LAST(perFrame) (PC_LAST_SUBTICK ? (perFrame) : 0)

/* This tick's part of a per-frame amount: the parts of a frame sum to it exactly. */
#define PC_SHARE(perFrame)                                                                                    \
    ((int)((long long)(perFrame) * (g_pcSubtick + 1) / g_pcFrameTicks -                                       \
           (long long)(perFrame) * g_pcSubtick / g_pcFrameTicks))

/* A per-frame retention in Q12 (what share of a quantity a frame keeps) as a per-tick one: its
 * g_pcTicksPerFrame-th root, in integers so every machine gets the same. */
int PcTickRetention(int perFrameQ12);
/* `value` kept by a tick, rounded toward zero so that negative quantities die away as positive ones do. */
int PcDecay(int value, int retentionQ12);

/* A per-frame damping or approach written the console's way, `original`, and the same as a retention: the
 * original with one tick a frame (bit for bit, roundings included), the per-tick root otherwise.
 *   PC_DECAY_EX(v * r >> 12, v, r)               the value kept
 *   PC_APPROACH_EX(d / k, d, PC_KEEP(k))         the step towards a target d away (x += d / k) */
#define PC_DECAY_EX(original, value, retentionQ12) \
    (g_pcFrameTicks == 1 ? (original) : PcDecay((value), (retentionQ12)))
#define PC_APPROACH_EX(original, distance, retentionQ12) \
    (g_pcFrameTicks == 1 ? (original) : (distance) - PcDecay((distance), (retentionQ12)))
/* The retention of an approach that closes 1/k of the distance a frame. */
#define PC_KEEP(k) ((4096 * ((k) - 1) + (k) / 2) / (k))

/* Whether a game state runs every tick (port/src/substep.c lists them). */
int PcStateSubsteps(int gameState);

/* Ends a tick that is not the frame's last: emulated time moves to the tick's share of the frame, and the
 * host shows what was drawn (port/src/kernel.c). */
void KernelSubtickEnd(void);

#endif
