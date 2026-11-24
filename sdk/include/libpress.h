/* PsyQ-compatible <libpress.h> for the PC port: the movie decoder, over the port's software MDEC. */

#ifndef PORT_LIBPRESS_H
#define PORT_LIBPRESS_H

#include <sys/types.h>

typedef u_short DECDCTTAB[34816];

typedef struct {
    u_char iq_y[64];
    u_char iq_c[64];
    short dct[64];
} DECDCTENV;

void DecDCTReset(int mode);
DECDCTENV *DecDCTGetEnv(DECDCTENV *env);
DECDCTENV *DecDCTPutEnv(DECDCTENV *env);
int DecDCTBufSize(u_long *bs);
int DecDCTvlc(u_long *bs, u_long *buf);
int DecDCTvlc2(u_long *bs, u_long *buf, DECDCTTAB table);
void DecDCTin(u_long *buf, int mode);
void DecDCTout(u_long *buf, int size);
int DecDCTinSync(int mode);
int DecDCToutSync(int mode);
int DecDCTinCallback(void (*func)(void));
int DecDCToutCallback(void (*func)(void));

#endif
