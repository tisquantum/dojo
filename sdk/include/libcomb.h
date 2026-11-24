/* PsyQ-compatible <libcomb.h> for the PC port: the link cable driver. */

#ifndef PORT_LIBCOMB_H
#define PORT_LIBCOMB_H

/* Status and control bits of the serial port as _comb_control reports and takes them. */
#define COMB_CTS     0x100
#define COMB_DSR     0x80
#define COMB_BIT_DTR 0x1
#define COMB_BIT_RTS 0x2

void AddCOMB(void);
void DelCOMB(void);
void ChangeClearSIO(long val);
long _comb_control(unsigned long cmd, unsigned long arg, unsigned long param);

#endif
