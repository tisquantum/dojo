/* Included first by every unit whose data is simulation state (the game, the decompiled libraries, the
 * translated assembly and the port's emulated hardware): their variables go to the `$sim` parts of .data and
 * .bss, which the linker gathers in one run between the markers savestate.c places, so a save state is those
 * two runs of memory (with main RAM, the I/O registers and the game's stack). */

#pragma clang section bss = ".bss$sim" data = ".data$sim"
