/* Simulation substeps (pc_substep.h). */

#include "pc_substep.h"

/* The setting is not state: an online match's machines agree on it. */
#pragma clang section bss = ".bss$render" data = ".data$render"
int g_pcTicksPerFrame = 1;
int g_pcRules; /* pc_rules.h: agreed the same way */
#pragma clang section bss = ".bss$sim" data = ".data$sim"

/* A frame that doesn't substep is one tick, 0 of 1, whatever the setting: such frames are the same state at
 * every rate. */
int g_pcFrameTicks = 1;
int g_pcSubtick;

/* The states whose code is written for ticks (see pc_substep.h): combat and the scenes the fighters play
 * in (the attract mode's bout, the replay, the victory and continue screens, the Slash-clear and approach
 * cutscenes), the opening, the story's cutscenes and its prologue. Not the character select, where online
 * matches start and return: its state must be the same at every rate. */
int PcStateSubsteps(int gameState)
{
    switch (gameState) {
    case 2:  /* GAME_STATE_UPDATE_COMBAT */
    case 4:  /* GAME_STATE_UPDATE_OPENING */
    case 26: /* GAME_STATE_UPDATE_STORY_CUTSCENE */
    case 38: /* GAME_STATE_UPDATE_STORY_PROLOGUE */
    case 6:  /* GAME_STATE_UPDATE_DEMO_BATTLE */
    case 14: /* GAME_STATE_UPDATE_SLASH_CLEAR_CUTSCENE */
    case 18: /* GAME_STATE_UPDATE_BATTLE_REPLAY */
    case 20: /* GAME_STATE_UPDATE_VICTORY_SCENE */
    case 28: /* GAME_STATE_UPDATE_CONTINUE_SCREEN */
    case 48: /* GAME_STATE_UPDATE_APPROACH_CUTSCENE */
        return 1;
    }
    return 0;
}

/* The n-th root of r/4096, in Q12: the largest y with (y/4096)^n <= r/4096, found by bisection in 64-bit
 * integers. Retentions are 0 to 4096; the results are cached per value for the current n. */
static int RootQ12(int r, int n)
{
    int lo = 0, hi = 4096;
    if (r <= 0) {
        return 0;
    }
    if (r >= 4096) {
        return 4096;
    }
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2, i;
        unsigned long long p = 4096;
        for (i = 0; i < n; i++) {
            p = p * (unsigned)mid / 4096;
        }
        if (p <= (unsigned long long)r) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

/* A cache of results, not state. */
#pragma clang section bss = ".bss$render" data = ".data$render"
static int sRootCacheN;
static short sRootCache[4097];
#pragma clang section bss = ".bss$sim" data = ".data$sim"

int PcTickRetention(int perFrameQ12)
{
    int n = g_pcFrameTicks;
    if (n <= 1) {
        return perFrameQ12;
    }
    if (perFrameQ12 < 0 || perFrameQ12 > 4096) {
        return RootQ12(perFrameQ12, n);
    }
    if (sRootCacheN != n) {
        int i;
        for (i = 0; i <= 4096; i++) {
            sRootCache[i] = -1;
        }
        sRootCacheN = n;
    }
    if (sRootCache[perFrameQ12] < 0) {
        sRootCache[perFrameQ12] = (short)RootQ12(perFrameQ12, n);
    }
    return sRootCache[perFrameQ12];
}

int PcDecay(int value, int retentionQ12)
{
    long long kept = (long long)(value < 0 ? -value : value) * PcTickRetention(retentionQ12) >> 12;
    return value < 0 ? -(int)kept : (int)kept;
}
