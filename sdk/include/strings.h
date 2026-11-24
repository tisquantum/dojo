/* PsyQ-compatible <strings.h> for the PC port. */

#ifndef PORT_STRINGS_H
#define PORT_STRINGS_H

#include <string.h>

#define bcopy(src, dst, n) memmove((dst), (src), (n))
#define bzero(p, n)        memset((p), 0, (n))
#define bcmp(a, b, n)      memcmp((a), (b), (n))

#endif
