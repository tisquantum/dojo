/* The part of LIBGTE the port writes itself: InitGeom, which on the console also patches the BIOS's
 * exception handler and enables the coprocessor. Its register defaults are the library's (msc00.s). */

#include <libgte.h>
#include "gte_soft.h"

void InitGeom(void)
{
    GteWriteCtrl(29, 341);         /* ZSF3: a third of 1024 */
    GteWriteCtrl(30, 256);         /* ZSF4: a quarter */
    GteWriteCtrl(26, 1000);        /* H */
    GteWriteCtrl(27, -4194);       /* DQA */
    GteWriteCtrl(28, 0x1400000);   /* DQB */
    GteWriteCtrl(24, 0);           /* OFX */
    GteWriteCtrl(25, 0);           /* OFY */
}
