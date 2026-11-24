/* PsyQ-compatible <libsnd.h> for the PC port: the sound library's VAB records, modes and routines. The
 * decompiled LIBSND runs unchanged over the port's emulated sound processor. */

#ifndef PORT_LIBSND_H
#define PORT_LIBSND_H

#include "psxport.h"

#define SS_NOTICK       0x1000
#define SS_NOTICK0      0
#define SS_TICK60       1
#define SS_TICK240      2
#define SS_TICK120      3
#define SS_TICK50       4
#define SS_TICKVSYNC    5
#define SS_TICKMODE_MAX 6

#define SS_SEQ_TABSIZ 172

#define SS_SERIAL_A 0
#define SS_SERIAL_B 1
#define SS_MIX      0
#define SS_REV      1
#define SS_SOFF     0
#define SS_SON      1

#define SSPLAY_INFINITY 0

/* MIDI controller handler slots of SsFCALL.control. */
#define CC_NUMBER     0
#define CC_BANKCHANGE 1
#define CC_DATAENTRY  2
#define CC_MAINVOL    3
#define CC_PANPOT     4
#define CC_EXPRESSION 5
#define CC_DAMPER     6
#define CC_NRPN1      7
#define CC_NRPN2      8
#define CC_RPN1       9
#define CC_RPN2       10
#define CC_EXTERNAL   11
#define CC_RESETALL   12

#define SS_IMEDIATE       0
#define SS_WAIT_COMPLETED 1

#define SS_REV_TYPE_OFF      0
#define SS_REV_TYPE_ROOM     1
#define SS_REV_TYPE_STUDIO_A 2
#define SS_REV_TYPE_STUDIO_B 3
#define SS_REV_TYPE_STUDIO_C 4
#define SS_REV_TYPE_HALL     5
#define SS_REV_TYPE_SPACE    6
#define SS_REV_TYPE_ECHO     7
#define SS_REV_TYPE_DELAY    8
#define SS_REV_TYPE_PIPE     9

typedef struct VabHdr {
    long form;
    long ver;
    long id;
    unsigned long fsize;
    unsigned short reserved0;
    unsigned short ps;
    unsigned short ts;
    unsigned short vs;
    unsigned char mvol;
    unsigned char pan;
    unsigned char attr1;
    unsigned char attr2;
    unsigned long reserved1;
} VabHdr;

typedef struct ProgAtr {
    unsigned char tones;
    unsigned char mvol;
    unsigned char prior;
    unsigned char mode;
    unsigned char mpan;
    char reserved0;
    short attr;
    unsigned long reserved1;
    unsigned long reserved2;
} ProgAtr;

typedef struct VagAtr {
    unsigned char prior;
    unsigned char mode;
    unsigned char vol;
    unsigned char pan;
    unsigned char center;
    unsigned char shift;
    unsigned char min;
    unsigned char max;
    unsigned char vibW;
    unsigned char vibT;
    unsigned char porW;
    unsigned char porT;
    unsigned char pbmin;
    unsigned char pbmax;
    unsigned char reserved1;
    unsigned char reserved2;
    unsigned short adsr1;
    unsigned short adsr2;
    short prog;
    short vag;
    short reserved[4];
} VagAtr;

typedef struct {
    unsigned short left;
    unsigned short right;
} SndVolume;

typedef struct {
    void (*noteon)();
    void (*programchange)();
    void (*pitchbend)();
    void (*metaevent)();
    void (*control[13])();
    void (*ccentry[20])();
} _SsFCALL;

typedef void (*SsMarkCallbackProc)(short, short, short);

extern _SsFCALL SsFCALL;

short SsVabOpenHead(unsigned char *addr, short vabid);
short SsVabTransBody(unsigned char *addr, short vabid);
short SsVabTransCompleted(short immediateFlag);
void SsVabClose(short vabid);
void SsInit(void);
void SsSetTableSize(char *table, short s_max, short t_max);
void SsSetTickMode(long tick_mode);
void SsStart(void);
void SsEnd(void);
void SsQuit(void);
void SsSeqCalledTbyT(void);
void SsSetMVol(short voll, short volr);
void SsSetStereo(void);
void SsSetSerialAttr(char s_num, char attr, char mode);
void SsSetSerialVol(char s_num, short voll, short volr);
char SsSetReservedVoice(char voices);
short SsUtKeyOnV(short voice, short vabId, short prog, short tone, short note, short fine, short voll,
                 short volr);
short SsUtKeyOffV(short voice);
short SsUtSetVVol(short vc, short voll, short volr);
void SsUtReverbOn(void);
void SsUtReverbOff(void);
short SsUtSetReverbType(short type);
void SsUtSetReverbDepth(short ldepth, short rdepth);

#endif
