/* PsyQ-compatible <sys/types.h> for the PC port. The game assumes a 32-bit long and pointer; the port builds
 * for a 32-bit target so both hold. */

#ifndef PORT_SYS_TYPES_H
#define PORT_SYS_TYPES_H

#include_next <sys/types.h>

typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef unsigned short ushort;
typedef unsigned int uint;

#ifndef NULL
#define NULL ((void *)0)
#endif

typedef char __port_long_is_32[(sizeof(long) == 4) ? 1 : -1];
typedef char __port_ptr_is_32[(sizeof(void *) == 4) ? 1 : -1];

#endif
