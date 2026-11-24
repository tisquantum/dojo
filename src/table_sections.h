/* For decompiled modules that are only constant tables (LIBPRESS's VLC tables): kept out of the simulation's
 * sections (sim_sections.h), so save states and their hashes leave them out. Included after it. */
#pragma clang section bss = ".bss$tables" data = ".data$tables"
