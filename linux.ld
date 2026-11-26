/* The Linux build's additions to the linker's own layout (CMakeLists.txt): the simulation's data and its
 * zero-filled data (src/sim_sections.h) each gathered in one run between two symbols, which is what a save
 * state copies (src/savestate.c). On Windows the linker orders the `$` sections itself. */

SECTIONS
{
    .data.dojo_sim : ALIGN(16)
    {
        dojo_sim_data_begin = .;
        *(.data$sim)
        . = ALIGN(16);
        dojo_sim_data_end = .;
    }
}
INSERT AFTER .data;

SECTIONS
{
    .bss.dojo_sim (NOLOAD) : ALIGN(16)
    {
        dojo_sim_bss_begin = .;
        *(.bss$sim)
        . = ALIGN(16);
        dojo_sim_bss_end = .;
    }
}
INSERT AFTER .bss;
