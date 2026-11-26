/* The memory cards for the port: each slot is a folder (saves/bu00, saves/bu10) holding one file per save,
 * a whole number of the card's 8 KB blocks as the BIOS allocates them. The card BIOS completes at once, so
 * every request delivers its events before returning, as the game's polling expects: a read or write both the
 * BIOS's (SwCARD) and the card's (HwCARD), which the game waits on after an O_NOWAIT transfer. */

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define MakeFolder(path) _mkdir(path)
#else
#include <dirent.h>
#include <fnmatch.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#define MakeFolder(path) mkdir(path, 0777)
#define _access access

/* The part of the Windows runtime's _findfirst the card uses, over a directory listing and fnmatch (whose
 * '?' and '*' are the BIOS's too): one search open at a time per handle. */
#define _A_SUBDIR 0x10
struct _finddata_t {
    unsigned attrib;
    long size;
    char name[260];
};
typedef struct FindState {
    DIR *dir;
    char folder[260], pattern[260];
} FindState;

static int _findnext(intptr_t handle, struct _finddata_t *f)
{
    FindState *s = (FindState *)handle;
    struct dirent *e;
    while ((e = readdir(s->dir)) != NULL) {
        char path[600];
        struct stat st;
        if (e->d_name[0] == '.' || fnmatch(s->pattern, e->d_name, 0) != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", s->folder, e->d_name);
        if (stat(path, &st) != 0) {
            continue;
        }
        f->attrib = S_ISDIR(st.st_mode) ? _A_SUBDIR : 0;
        f->size = (long)st.st_size;
        snprintf(f->name, sizeof(f->name), "%s", e->d_name);
        return 0;
    }
    return -1;
}

static int _findclose(intptr_t handle)
{
    FindState *s = (FindState *)handle;
    closedir(s->dir);
    free(s);
    return 0;
}

static intptr_t _findfirst(const char *spec, struct _finddata_t *f)
{
    FindState *s = calloc(1, sizeof(*s));
    const char *slash = strrchr(spec, '/');
    if (!s || !slash) {
        free(s);
        return -1;
    }
    snprintf(s->folder, sizeof(s->folder), "%.*s", (int)(slash - spec), spec);
    snprintf(s->pattern, sizeof(s->pattern), "%s", slash + 1);
    s->dir = opendir(s->folder);
    if (!s->dir) {
        free(s);
        return -1;
    }
    if (_findnext((intptr_t)s, f) != 0) {
        _findclose((intptr_t)s);
        return -1;
    }
    return (intptr_t)s;
}
#endif
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
    MakeFolder("saves");
    MakeFolder(dir);
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
    DeliverEvent(HwCARD, EvSpIOE); /* a read or write with O_NOWAIT is waited on by the card's event */
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
    DeliverEvent(HwCARD, EvSpIOE); /* the game's save waits on this one (memcard_io.c WaitHwCardEvent) */
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
    /* All of the name's bytes, as the card's directory has them: a 20-character name (the game's own save,
     * BASCUS-94180BUSHIDO1) fills the field with no end, and the game compares all 20. */
    strncpy(dir->name, sFind.name, sizeof(dir->name));
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
