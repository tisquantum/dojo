/* PsyQ-compatible <sys/file.h> for the PC port: the BIOS file open modes. */

#ifndef PORT_SYS_FILE_H
#define PORT_SYS_FILE_H

#define FREAD   0x0001
#define FWRITE  0x0002
#define FNBLOCK 0x0004
#define FRLOCK  0x0010
#define FWLOCK  0x0020
#define FAPPEND 0x0100
#define FCREAT  0x0200
#define FTRUNC  0x0400
#define FSCAN   0x1000
#define FRCOM   0x2000
#define FNBUF   0x4000
#define FASYNC  0x8000

#define O_RDONLY FREAD
#define O_WRONLY FWRITE
#define O_RDWR   (FREAD | FWRITE)
#define O_CREAT  FCREAT
#define O_NOBUF  FNBUF
#define O_NBLOCK FNBLOCK
#define O_NOWAIT FASYNC

#endif
