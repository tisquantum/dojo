/* A shadow call stack of the game's functions, kept when the port is built with DOJO_TRACE (the game is
 * then compiled with -finstrument-functions). The crash handler prints it: it survives a smashed stack. */

#include <stdio.h>

#define TRACE_DEPTH 256

static void *sTraceStack[TRACE_DEPTH];
static int sTraceDepth;

__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void *fn, void *site)
{
    (void)site;
    if (sTraceDepth < TRACE_DEPTH) {
        sTraceStack[sTraceDepth] = fn;
    }
    sTraceDepth++;
}

__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void *fn, void *site)
{
    (void)fn;
    (void)site;
    if (sTraceDepth > 0) {
        sTraceDepth--;
    }
}

__attribute__((no_instrument_function)) void TracePrint(FILE *out)
{
    int i, n = sTraceDepth < TRACE_DEPTH ? sTraceDepth : TRACE_DEPTH;
    fprintf(out, "trace:");
    for (i = n - 1; i >= 0; i--) {
        fprintf(out, " %08lx", (unsigned long)(size_t)sTraceStack[i]);
    }
    fprintf(out, "\n");
}
