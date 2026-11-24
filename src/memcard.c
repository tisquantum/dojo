/* The memory cards for the port: each slot is a folder (saves/bu00, saves/bu10) holding one file per save,
 * a whole number of the card's 8 KB blocks as the BIOS allocates them. The card BIOS completes at once, so
 * every request delivers its event before returning, as the game's polling expects. */

#include <direct.h>
#include <stdio.h>
#include <string.h>
#include <io.h>
#include <kernel.h>
#include "netplay.h"

#undef open
#undef close
#undef read
#undef write
#undef lseek
#undef firstfile
#undef nextfile
#undef erase
#undef format
#undef delete

#define CARD_BLOCK_SIZE  8192
#define CARD_BLOCKS      15
#define CARD_FILE_MAX    8

static struct {
    FILE *file;
    long size;
} sFiles[CARD_FILE_MAX];

static struct _finddata_t sFind;
static intptr_t sFindHandle = -1;
static char sFindDir[32];

/* "bu00:NAME" to "saves/bu00/NAME"; returns 0 for a name that is not on a card. */
static int HostPath(const char *devname, char *out, size_t size, char *dir, size_t dirSize)
{
    const char *colon = strchr(devname, ':');
    if (strncmp(devname, "bu", 2) != 0 || colon == NULL || colon - devname != 4) {
        return 0;
    }
    snprintf(dir, dirSize, "saves/%.*s", (int)(colon - devname), devname);
    _mkdir("saves");
    _mkdir(dir);
    snprintf(out, size, "%s/%s", dir, colon + 1);
    return 1;
}

long PsxOpen(const char *devname, unsigned long flag)
{
    char path[260], dir[32];
    int fd;
    FILE *f;

    if (NetplayCardsOff() || !HostPath(devname, path, sizeof(path), dir, sizeof(dir))) {
        return -1;
    }
    for (fd = 0; fd < CARD_FILE_MAX && sFiles[fd].file; fd++) {
    }
    if (fd == CARD_FILE_MAX) {
        return -1;
    }
    if (flag & FCREAT) {
        long blocks = (long)(flag >> 16);
        static const char zero[CARD_BLOCK_SIZE];
        long i;
        if (_access(path, 0) == 0) {
            return -1; /* the BIOS will not create over an existing file */
        }
        f = fopen(path, "w+b");
        if (!f) {
            return -1;
        }
        for (i = 0; i < (blocks ? blocks : 1); i++) {
            fwrite(zero, 1, sizeof(zero), f);
        }
        fclose(f);
    }
    f = fopen(path, (flag & (FWRITE | FCREAT)) ? "r+b" : "rb");
    if (!f) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    sFiles[fd].size = ftell(f);
    fseek(f, 0, SEEK_SET);
    sFiles[fd].file = f;
    return fd;
}

long PsxClose(long fd)
{
    if (fd < 0 || fd >= CARD_FILE_MAX || !sFiles[fd].file) {
        return -1;
    }
    fclose(sFiles[fd].file);
    sFiles[fd].file = NULL;
    return fd;
}

long PsxRead(long fd, void *buf, long n)
{
    long got;
    if (fd < 0 || fd >= CARD_FILE_MAX || !sFiles[fd].file) {
        return -1;
    }
    got = (long)fread(buf, 1, n, sFiles[fd].file);
    DeliverEvent(SwCARD, EvSpIOE);
    return got;
}

long PsxWrite(long fd, void *buf, long n)
{
    long put;
    if (fd < 0 || fd >= CARD_FILE_MAX || !sFiles[fd].file) {
        return -1;
    }
    put = (long)fwrite(buf, 1, n, sFiles[fd].file);
    fflush(sFiles[fd].file);
    DeliverEvent(SwCARD, EvSpIOE);
    return put;
}

long PsxLseek(long fd, long offset, long flag)
{
    if (fd < 0 || fd >= CARD_FILE_MAX || !sFiles[fd].file) {
        return -1;
    }
    fseek(sFiles[fd].file, offset, flag == 0 ? SEEK_SET : (flag == 1 ? SEEK_CUR : SEEK_END));
    return ftell(sFiles[fd].file);
}

static struct DIRENTRY *FillEntry(struct DIRENTRY *dir)
{
    memset(dir, 0, sizeof(*dir));
    strncpy(dir->name, sFind.name, sizeof(dir->name) - 1);
    dir->size = (long)sFind.size;
    return dir;
}

struct DIRENTRY *PsxFirstFile(char *name, struct DIRENTRY *dir)
{
    char path[260], pattern[300];
    const char *colon = strchr(name, ':');
    if (sFindHandle != -1) {
        _findclose(sFindHandle);
        sFindHandle = -1;
    }
    if (NetplayCardsOff() || !HostPath(name, path, sizeof(path), sFindDir, sizeof(sFindDir))) {
        return NULL;
    }
    /* The game's patterns use the BIOS's '?' and '*'; the host's _findfirst takes the same. */
    snprintf(pattern, sizeof(pattern), "%s/%s", sFindDir, (colon && colon[1]) ? colon + 1 : "*");
    sFindHandle = _findfirst(pattern, &sFind);
    if (sFindHandle == -1) {
        return NULL;
    }
    while (sFind.attrib & _A_SUBDIR) {
        if (_findnext(sFindHandle, &sFind) != 0) {
            return NULL;
        }
    }
    return FillEntry(dir);
}

struct DIRENTRY *PsxNextFile(struct DIRENTRY *dir)
{
    if (sFindHandle == -1) {
        return NULL;
    }
    do {
        if (_findnext(sFindHandle, &sFind) != 0) {
            _findclose(sFindHandle);
            sFindHandle = -1;
            return NULL;
        }
    } while (sFind.attrib & _A_SUBDIR);
    return FillEntry(dir);
}

long PsxErase(char *name)
{
    char path[260], dir[32];
    if (!HostPath(name, path, sizeof(path), dir, sizeof(dir))) {
        return 0;
    }
    return remove(path) == 0;
}

long PsxDelete(char *name)
{
    return PsxErase(name);
}

long PsxFormat(char *fs)
{
    char path[260], dir[32], pattern[300];
    struct _finddata_t f;
    intptr_t h;
    if (!HostPath(fs, path, sizeof(path), dir, sizeof(dir))) {
        return 0;
    }
    snprintf(pattern, sizeof(pattern), "%s/*", dir);
    h = _findfirst(pattern, &f);
    if (h != -1) {
        do {
            if (!(f.attrib & _A_SUBDIR)) {
                snprintf(path, sizeof(path), "%s/%s", dir, f.name);
                remove(path);
            }
        } while (_findnext(h, &f) == 0);
        _findclose(h);
    }
    return 1;
}

/* The card BIOS: a formatted card is always present in both slots, except in an online match, where
 * there is none. */

void InitCARD(long pad_enable)
{
    (void)pad_enable;
}

long StartCARD(void)
{
    return 1;
}

long StopCARD(void)
{
    return 1;
}

long _card_info(long chan)
{
    (void)chan;
    DeliverEvent(SwCARD, NetplayCardsOff() ? EvSpTIMOUT : EvSpIOE);
    return 1;
}

long _card_load(long chan)
{
    (void)chan;
    DeliverEvent(SwCARD, NetplayCardsOff() ? EvSpTIMOUT : EvSpIOE);
    return 1;
}

long _card_clear(long chan)
{
    (void)chan;
    DeliverEvent(HwCARD, NetplayCardsOff() ? EvSpTIMOUT : EvSpIOE);
    return 1;
}
