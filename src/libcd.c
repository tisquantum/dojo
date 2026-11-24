/* LIBCD for the port: a virtual drive over the player's MODE2/2352 disc image.
 *
 * The game drives the drive at command level (src/cd.c): it sets the mode and position, starts ReadN with a
 * ready callback that takes each sector with CdGetSector, and polls CdSync for command completion. Commands
 * complete at once. Reading delivers sectors at each vertical blank (kernel.c calls CdVerticalBlank),
 * faster than the console's drive; XA streams (ReadS) advance at the console's rate, since the game times
 * the speech and music they carry by their position. */

#include <stdio.h>
#include <string.h>
#include <libcd.h>
#include "cdrom.h"
#include "host.h"
#include "kernel_port.h"

#define RAW_SECTOR       2352
#define SECTOR_BODY      2340 /* what a Size1 read delivers: the sector past its 12-byte sync */
#define DATA_SECTORS_PER_FIELD 32
#define XA_SECTORS_PER_SECOND  150

/* The open image is this machine's; what the drive does with it is the simulation's. */
#pragma clang section bss = ".bss$render" data = ".data$render"
static FILE *sDisc;
static long sDiscSectors;
#pragma clang section bss = ".bss$sim" data = ".data$sim"

static struct {
    u_char mode;
    CdlFILTER filter;
    int targetLba;     /* CdlSetloc */
    int lba;           /* the head's position */
    int reading;       /* 1 ReadN, 2 ReadS, 3 ReadS streaming a movie (CdRead2) */
    int xaAccumulator; /* XA position in 1/60ths of a sector */
    u_char syncStatus;
    u_char syncResult[8];
    u_char readyStatus;
    u_char readyResult[8];
    CdlCB readyCallback;
    CdlCB syncCallback;
    u_char sector[SECTOR_BODY];
    int sectorCursor;
    CdlATV mix;
    u_char lastCommand;
} sCd;

u_long StCdIntrFlag;
static void StreamSector(const unsigned char *raw);

static u_char ToBcd(int v)
{
    return (u_char)((v / 10) << 4 | (v % 10));
}

static int FromBcd(u_char v)
{
    return (v >> 4) * 10 + (v & 15);
}

CdlLOC *CdIntToPos(int i, CdlLOC *p)
{
    i += 150;
    p->minute = ToBcd(i / (75 * 60));
    p->second = ToBcd((i / 75) % 60);
    p->sector = ToBcd(i % 75);
    return p;
}

int CdPosToInt(CdlLOC *p)
{
    return (FromBcd(p->minute) * 60 + FromBcd(p->second)) * 75 + FromBcd(p->sector) - 150;
}

static int OpenDisc(void)
{
    if (sDisc) {
        return 1;
    }
    sDisc = fopen(HostDiscPath(), "rb");
    if (!sDisc) {
        HostFatal("cannot open the disc image %s", HostDiscPath());
    }
    fseek(sDisc, 0, SEEK_END);
    sDiscSectors = ftell(sDisc) / RAW_SECTOR;
    return 1;
}

int CdReadRawSector(int lba, unsigned char *out)
{
    OpenDisc();
    if (lba < 0 || lba >= sDiscSectors) {
        memset(out, 0, RAW_SECTOR);
        return 0;
    }
    fseek(sDisc, (long)lba * RAW_SECTOR, SEEK_SET);
    return fread(out, 1, RAW_SECTOR, sDisc) == RAW_SECTOR;
}

static u_char DriveStatus(void)
{
    u_char status = 0x02; /* the motor turns */
    if (sCd.reading) {
        status |= 0x20;
    }
    return status;
}

static void Complete(u_char *result)
{
    sCd.syncStatus = CdlComplete;
    sCd.syncResult[0] = DriveStatus();
    if (result) {
        result[0] = sCd.syncResult[0];
    }
}

static void Execute(u_char com, u_char *param, u_char *result)
{
    u_char local[8];
    memset(local, 0, sizeof(local));
    sCd.lastCommand = com;
    switch (com) {
    case CdlSetloc:
        sCd.targetLba = CdPosToInt((CdlLOC *)param);
        break;
    case CdlSetmode:
        sCd.mode = param[0];
        break;
    case CdlSetfilter:
        sCd.filter = *(CdlFILTER *)param;
        break;
    case CdlReadN:
    case CdlReadS:
        if (param) {
            sCd.targetLba = CdPosToInt((CdlLOC *)param);
        }
        sCd.lba = sCd.targetLba;
        sCd.reading = com == CdlReadN ? 1 : 2;
        sCd.xaAccumulator = 0;
        break;
    case CdlSeekL:
    case CdlSeekP:
        sCd.lba = sCd.targetLba;
        break;
    case CdlPause:
    case CdlStop:
    case CdlStandby:
        sCd.reading = 0;
        break;
    case CdlPlay:
        /* The disc has only its data track: there is no CD audio to play. */
        break;
    case CdlGetTN:
        local[1] = ToBcd(1);
        local[2] = ToBcd(1);
        break;
    case CdlGetlocP: {
        int rel = sCd.lba;
        CdlLOC abs;
        CdIntToPos(sCd.lba, &abs);
        local[0] = ToBcd(1);
        local[1] = ToBcd(1);
        local[2] = ToBcd(rel / (75 * 60));
        local[3] = ToBcd((rel / 75) % 60);
        local[4] = ToBcd(rel % 75);
        local[5] = abs.minute;
        local[6] = abs.second;
        local[7] = abs.sector;
        break;
    }
    default:
        break;
    }
    Complete(NULL);
    if (com != CdlGetlocP && com != CdlGetTN) {
        local[0] = sCd.syncResult[0];
    }
    memcpy(sCd.syncResult, local, sizeof(local));
    if (result) {
        memcpy(result, local, com == CdlGetlocP ? 8 : (com == CdlGetTN ? 3 : 1));
    }
}

int CdInit(void)
{
    OpenDisc();
    memset(&sCd, 0, sizeof(sCd));
    sCd.syncStatus = CdlComplete;
    return 1;
}

int CdReset(int mode)
{
    (void)mode;
    return CdInit();
}

void CdFlush(void)
{
    sCd.readyStatus = CdlNoIntr;
}

int CdSetDebug(int level)
{
    (void)level;
    return 0;
}

int CdControl(u_char com, u_char *param, u_char *result)
{
    Execute(com, param, result);
    return 1;
}

int CdControlB(u_char com, u_char *param, u_char *result)
{
    Execute(com, param, result);
    return 1;
}

int CdControlF(u_char com, u_char *param)
{
    Execute(com, param, NULL);
    return 1;
}

int CdSync(int mode, u_char *result)
{
    (void)mode;
    if (result) {
        memcpy(result, sCd.syncResult, 8);
    }
    return sCd.syncStatus;
}

int CdReady(int mode, u_char *result)
{
    int status = sCd.readyStatus;
    (void)mode;
    if (result) {
        memcpy(result, sCd.readyResult, 8);
    }
    sCd.readyStatus = CdlNoIntr;
    return status;
}

CdlCB CdReadyCallback(CdlCB func)
{
    CdlCB old = sCd.readyCallback;
    sCd.readyCallback = func;
    return old;
}

CdlCB CdSyncCallback(CdlCB func)
{
    CdlCB old = sCd.syncCallback;
    sCd.syncCallback = func;
    return old;
}

int CdGetSector(void *madr, int size)
{
    int bytes = size * 4;
    if (sCd.sectorCursor + bytes > SECTOR_BODY) {
        bytes = SECTOR_BODY - sCd.sectorCursor;
    }
    if (bytes > 0) {
        memcpy(madr, &sCd.sector[sCd.sectorCursor], bytes);
        sCd.sectorCursor += bytes;
    }
    return 1;
}

int CdMix(CdlATV *vol)
{
    sCd.mix = *vol;
    return 1;
}

int CdStatus(void)
{
    return DriveStatus();
}

int CdMode(void)
{
    return sCd.mode;
}

int CdLastCom(void)
{
    return sCd.lastCommand;
}

const CdlATV *CdAudioMix(void)
{
    return &sCd.mix;
}

int CdXaFilter(int *file, int *channel)
{
    *file = sCd.filter.file;
    *channel = sCd.filter.chan;
    return (sCd.mode & CdlModeSF) != 0;
}

/* Delivers the sector under the head to the ready callback and moves on. */
static void DeliverSector(void)
{
    unsigned char raw[RAW_SECTOR];
    CdReadRawSector(sCd.lba, raw);
    memcpy(sCd.sector, raw + 12, SECTOR_BODY);
    sCd.sectorCursor = 0;
    sCd.lba++;
    sCd.readyResult[0] = DriveStatus();
    if (sCd.readyCallback) {
        sCd.readyCallback(CdlDataReady, sCd.readyResult);
    } else {
        sCd.readyStatus = CdlDataReady;
    }
}

void CdVerticalBlank(void)
{
    if (sCd.reading == 1) {
        int i;
        for (i = 0; i < DATA_SECTORS_PER_FIELD && sCd.reading == 1; i++) {
            DeliverSector();
        }
    } else if (sCd.reading >= 2) {
        int speed = (sCd.mode & CdlModeSpeed) ? XA_SECTORS_PER_SECOND : XA_SECTORS_PER_SECOND / 2;
        sCd.xaAccumulator += speed;
        while (sCd.xaAccumulator >= 60 && sCd.reading >= 2) {
            unsigned char raw[RAW_SECTOR];
            sCd.xaAccumulator -= 60;
            CdReadRawSector(sCd.lba, raw);
            if (sCd.reading == 3) {
                StreamSector(raw);
            } else {
                CdXaSector(raw);
            }
            sCd.lba++;
        }
    }
}

/* Streaming (CdRead2, the St functions): the movie player's ring of sectors, filled as LIBCD's StCdInterrupt
 * fills it. The drive reads at the console's rate; each video sector's 32-byte header goes to the header
 * area and its 2016 bytes of picture data to the data area, frames whole and in order, rewinding to the start
 * when the next would not fit; a frame is complete when its last sector is in. The movie's interleaved XA
 * sectors play as they pass the head. The ring's layout and states are LIBCD's, so the game's reading of it
 * (StGetNext, StFreeRing) is as on the console. */

#define ST_HEADER_WORDS 8   /* a sector's header in the ring */
#define ST_SECTOR_WORDS 512 /* a sector: header and data */
#define ST_VERSION      0x0160
#define ST_FREE     0
#define ST_REWIND   1
#define ST_COMPLETE 2
#define ST_BUSY     3
#define ST_LOCK     4
/* Halfwords of a sector's header; the status takes the place of the version once the sector is in. */
#define ST_STATUS        0
#define ST_VER           0
#define ST_TYPE          1
#define ST_SECTOR_OFFSET 2
#define ST_SECTOR_SIZE   3
#define ST_FRAME_NO      4
#define SUBMODE_AUDIO    0x04
#define SUBMODE_DATA     0x08
#define STREAM_POLL_LINES 16 /* about what a poll of an empty ring takes the console */

static struct {
    u_long *ring;
    u_long size;
    int feeding;      /* CdRead2 until StUnSetRing */
    u_long idx1;      /* the slot the next sector fills */
    u_long idx2;      /* the first slot of the frame being filled */
    u_long idx3;      /* the slot StGetNext reads next */
    u_long startFlag, startFrame, endFrame;
    u_long frameNo;
    short sectorOffset;
    u_long channel, nextChannel;
    void (*frameDone)(void);
    void (*streamEnd)(void);
} sSt;

static volatile u_short *StSlotHeader(u_long slot)
{
    return (volatile u_short *)(sSt.ring + slot * ST_HEADER_WORDS);
}

static void StFreeSlots(u_long start, u_long count)
{
    u_long i;
    for (i = 0; i < count; i++) {
        StSlotHeader(start + i)[ST_STATUS] = ST_FREE;
    }
}

/* One sector of the stream, as StCdInterrupt takes it. */
static void StreamSector(const unsigned char *raw)
{
    const unsigned char *user = raw + 24;
    volatile u_short *header;
    StHEADER *full;

    if (raw[18] & SUBMODE_AUDIO) {
        if (sCd.mode & CdlModeRT) {
            CdXaSector(raw);
        }
        return;
    }
    if (!(raw[18] & SUBMODE_DATA) || !sSt.feeding || !sSt.ring) {
        return;
    }
    header = StSlotHeader(sSt.idx1);
    if (header[ST_STATUS] != ST_FREE) {
        return; /* the ring is full: the sector is lost */
    }
    memcpy((void *)header, user, ST_HEADER_WORDS * 4);
    full = (StHEADER *)header;
    full->loc.minute = raw[12];
    full->loc.second = raw[13];
    full->loc.sector = raw[14];
    full->loc.track = 0;

    if (sSt.startFlag == 1 && sSt.startFrame != 0) {
        if (sSt.startFrame != header[ST_FRAME_NO]) {
            header[ST_STATUS] = ST_FREE;
            return;
        }
        sSt.startFlag = 0;
    }
    if (header[ST_VER] != ST_VERSION || ((header[ST_TYPE] >> 10) & 0x1F) != sSt.channel) {
        header[ST_STATUS] = ST_FREE;
        return;
    }
    if (sSt.sectorOffset != (short)header[ST_SECTOR_OFFSET] ||
        (sSt.frameNo != 0 && sSt.frameNo != header[ST_FRAME_NO])) {
        /* a sector missed: the frame so far is dropped */
        sSt.frameNo = 0;
        sSt.sectorOffset = 0;
        StFreeSlots(sSt.idx2, sSt.idx1 - sSt.idx2);
        sSt.idx1 = sSt.idx2;
        header[ST_STATUS] = ST_FREE;
        return;
    }
    if (header[ST_SECTOR_OFFSET] == 0) {
        sSt.sectorOffset = 0;
        sSt.frameNo = header[ST_FRAME_NO];
        if (sSt.endFrame != 0 && sSt.frameNo >= sSt.endFrame) {
            sSt.frameNo = 0;
            StFreeSlots(sSt.idx2, sSt.idx1 - sSt.idx2);
            sSt.idx1 = sSt.idx2;
            header[ST_STATUS] = ST_FREE;
            sSt.startFlag = 1;
            if (sSt.streamEnd) {
                sSt.streamEnd();
            }
            return;
        }
        if (sSt.size - sSt.idx1 - 1 < header[ST_SECTOR_SIZE]) {
            if (sSt.endFrame == 0) {
                header[ST_STATUS] = ST_REWIND;
                sSt.startFlag = 1;
                if (sSt.streamEnd) {
                    sSt.streamEnd();
                }
                return;
            }
            if (StSlotHeader(0)[ST_STATUS] != ST_FREE) {
                header[ST_STATUS] = ST_FREE;
                return;
            }
            header[ST_STATUS] = ST_REWIND;
            sSt.idx1 = 0;
            memcpy(sSt.ring, (const void *)header, ST_HEADER_WORDS * 4);
            header = StSlotHeader(0);
        }
        sSt.idx2 = sSt.idx1;
    }
    sSt.sectorOffset++;
    memcpy(sSt.ring + sSt.size * ST_HEADER_WORDS + sSt.idx1 * (ST_SECTOR_WORDS - ST_HEADER_WORDS),
           user + ST_HEADER_WORDS * 4, (ST_SECTOR_WORDS - ST_HEADER_WORDS) * 4);
    header[ST_STATUS] = ST_BUSY;
    sSt.idx1++;
    if (header[ST_SECTOR_SIZE] - 1 == header[ST_SECTOR_OFFSET]) {
        /* the frame's last sector: the frame is complete */
        sSt.sectorOffset = 0;
        sSt.frameNo = 0;
        sSt.channel = sSt.nextChannel;
        StSlotHeader(sSt.idx2)[ST_STATUS] = ST_COMPLETE;
        sSt.idx2 = sSt.idx1;
        if (sSt.frameDone) {
            sSt.frameDone();
        }
    }
}

int CdRead2(long mode)
{
    u_char param[4];
    param[0] = (u_char)mode;
    CdControl(CdlSetmode, param, 0);
    sSt.feeding = (mode & CdlModeStream) != 0;
    CdControl(CdlReadS, 0, 0);
    sCd.reading = 3;
    return 1;
}

void StSetRing(u_long *ring_addr, u_long ring_size)
{
    sSt.ring = ring_addr;
    sSt.size = ring_size;
    StClearRing();
}

void StClearRing(void)
{
    sSt.idx1 = sSt.idx2 = sSt.idx3 = 0;
    if (sSt.ring) {
        StFreeSlots(0, sSt.size);
    }
    StCdIntrFlag = 0;
    sSt.sectorOffset = 0;
    sSt.frameNo = 0;
}

void StUnSetRing(void)
{
    sSt.feeding = 0;
}

void StSetStream(u_long mode, u_long start_frame, u_long end_frame, void (*func1)(void), void (*func2)(void))
{
    (void)mode; /* 24-bit streams wait for the decoder's output DMA; the port's is done at once */
    sSt.startFlag = 1;
    sSt.startFrame = start_frame;
    sSt.endFrame = end_frame;
    sSt.frameDone = func1;
    sSt.streamEnd = func2;
    sSt.channel = sSt.nextChannel = 0;
    sSt.sectorOffset = 0;
    sSt.frameNo = 0;
}

u_long StFreeRing(u_long *base)
{
    int slot = (int)((base - (sSt.ring + sSt.size * ST_HEADER_WORDS)) / (ST_SECTOR_WORDS - ST_HEADER_WORDS));
    volatile u_short *header = StSlotHeader((u_long)slot);
    int count = ((StHEADER *)header)->nSectors, i;
    if (header[ST_STATUS] != ST_LOCK) {
        return 1;
    }
    for (i = 0; i < count; i++) {
        StSlotHeader((u_long)(slot + i))[ST_STATUS] = ST_FREE;
    }
    sSt.idx3 = (u_long)(slot + count);
    return 0;
}

u_long StGetNext(u_long **addr, u_long **header)
{
    volatile u_short *ring_header;
    /* A call into the library: the interrupts raised meanwhile (the decoder's output) are taken, as the
     * console takes them while the player gets on with this. */
    KernelPollInterrupts();
    if (!sSt.ring) {
        return 1;
    }
    ring_header = StSlotHeader(sSt.idx3);
    if (ring_header[ST_STATUS] == ST_REWIND) {
        sSt.idx3 = 0;
        if (sSt.endFrame) {
            ring_header[ST_STATUS] = ST_FREE;
        }
        ring_header = StSlotHeader(sSt.idx3);
    }
    if (ring_header[ST_STATUS] != ST_COMPLETE) {
        /* The player polls until the frame is in: the drive reads on meanwhile. */
        KernelPollWait(STREAM_POLL_LINES);
        return 1;
    }
    ring_header[ST_STATUS] = ST_LOCK;
    *addr = sSt.ring + sSt.size * ST_HEADER_WORDS + sSt.idx3 * (ST_SECTOR_WORDS - ST_HEADER_WORDS);
    *header = (u_long *)ring_header;
    return 0;
}
