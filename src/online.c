/* Online play through the matchmaking server (online.h).
 *
 * One UDP socket talks to the server and to the other player. Server messages are text lines ("BBMM ...",
 * port/server/matchmaker.py has the protocol); the other player is reached directly when the two routers let
 * it (each side sends to the other's public address, which the server tells it, and goes direct once the
 * other answers), else through the server's relay ("BBRL" + match id + packet). Before a match the two
 * exchange a handshake ("BBPP ..." text: punch, hello with each one's rate, input delay and the hash of the
 * starting state, ready); then the rollback protocol (netplay.c) runs over the chosen path.
 *
 * A private match can also go without the server, player to player: one hosts on a UDP port (forwarded on
 * the router to play over the internet) and the other joins by address; the handshake is the same, with no
 * relay to fall back on.
 *
 * King of the Hill can be watched. The players of a hill's match send the server its inputs as both have
 * settled them; those waiting in line pull the log and run the same fight from the same starting state, a
 * moment behind: the game is deterministic, so they see the real match, drawn and heard by their own machine. */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "online.h"
#include "netplay.h"
#include "savestate.h"
#include "settings.h"
#include "host.h"
#include "context.h"
#include "kernel_port.h"
#include "cdrom.h"
#include "spu.h"
#include "pc_substep.h"

#define PROTOCOL        1
#define ONLINE_MARGIN   380  /* every online match runs the widest simulation; each display shows its part */
#define ONLINE_DELAY    2
#define PUNCH_MS        150
#define RELAY_AFTER_MS  1500 /* no direct answer by then: through the server */
#define SETUP_GIVE_UP_MS 20000
#define SILENCE_MS      6000
#define PING_MS         2000
#define SERVER_SILENCE_MS 20000
#define PREPARE_FIELDS_PER_CALL 120
#define SPECTATE_BATCH  120   /* fields a log packet carries */
#define PUSH_MS         100   /* how often a player sends the log's new fields */
#define WATCH_MS        100   /* how often a spectator asks for what it lacks */
#define WATCH_BEHIND    60    /* fields a spectator stays behind the newest it has, against stalls */
#define WATCH_CATCH_UP  180   /* further behind than this: run on unseen to catch up */
#define WATCH_STALL_MS  8000  /* nothing new for this long: stop watching */

/* Game states (include/game_state.h) the preparation and the end of a bout look for. */
#define GS_INIT_MAIN_MENU         7
#define GS_UPDATE_MAIN_MENU       8
#define GS_UPDATE_CHARACTER_SELECT 10
#define GS_INIT_BATTLE_REPLAY     17
#define GS_INIT_CONTINUE_SCREEN   27

#define PAD_START (1u << 11)
#define PAD_DOWN  (1u << 14)
#define PAD_CROSS (1u << 6)

static struct {
    OnlineState state;
    OnlineMode mode;
    int serverless;        /* player to player: no server, no relay */
    int hosting;           /* serverless, waiting for the other to join */
    int socketPort;        /* the port the socket is bound to, 0 for any */
    SOCKET sock;
    int wsa;
    struct sockaddr_in server;
    uint32_t lastServerMs, lastPingMs, connectStartMs;
    char publicAddress[64];
    char room[16];         /* a private room's code, or a private hill's */
    int queuePosition, queueLength;
    char king[40];
    int kingStreak;
    char waitText[160];

    /* The match being set up or played. */
    uint32_t matchId;
    int role;              /* 0: player one */
    struct sockaddr_in peer;
    char peerName[40];
    int direct;            /* the other machine answered directly */
    uint32_t setupStartMs, lastPunchMs;
    int haveHello, sentReady, peerReady;
    int peerTicks, peerDelay;
    uint64_t peerHash;
    int ticks, delay;
    int winnerSide;        /* the bout's winner seen settled, or -1 */
    int resultSent;
    uint32_t matchStartMs;

    /* The state matches start from, and the offline game put aside. */
    uint8_t *start;
    uint64_t startHash;
    int preparing;         /* 0 idle; else fields run so far + 1 */
    uint8_t *offline;
    int offlineSaved;
    int phase, phaseField; /* the preparation's walk through the menus */

    /* Session packets that came, for netplay's transport. */
    struct {
        int size;
        char data[512];
    } queue[64];
    int queueRead, queueWrite;

    /* A hill match being played: how much of its log the server has. */
    uint32_t pushAcked;
    uint32_t lastPushMs;

    /* Watching a hill's match. */
    struct {
        int on;            /* a match to watch has been announced */
        int running;       /* its starting state is loaded and fields are running */
        uint32_t matchId;
        char names[2][40];
        uint32_t *log;
        uint32_t have, capacity;
        uint32_t total;    /* the log's length on the server, as last heard */
        uint32_t next;     /* the next field to run */
        int ticks;
        int ended;         /* the match is over: run what is left, then stop */
        uint32_t lastAskMs, lastNewMs;
    } W;

    char message[160];
    int hasMessage;
} O;

#pragma pack(push, 1)
typedef struct SpectateHeader {
    char magic[4];         /* "BBSP" */
    uint32_t matchId;
    uint32_t first;
    uint16_t count;
    uint16_t ticks;
    uint32_t total;
} SpectateHeader;
#pragma pack(pop)

static uint32_t NowMs(void)
{
    return (uint32_t)SDL_GetTicks();
}

static void Message(const char *text)
{
    snprintf(O.message, sizeof(O.message), "%s", text);
    O.hasMessage = 1;
    fprintf(stderr, "online: %u ms: %s\n", (unsigned)SDL_GetTicks(), text);
}

const char *OnlineTakeMessage(void)
{
    if (!O.hasMessage) {
        return NULL;
    }
    O.hasMessage = 0;
    return O.message;
}

static uint64_t DiscIdentity(void)
{
    unsigned char sector[2352];
    uint64_t h = 0xCBF29CE484222325ull;
    int lba, i;
    for (lba = 16; lba < 40; lba++) {
        CdReadRawSector(lba, sector);
        for (i = 0; i < 2352; i++) {
            h = (h ^ sector[i]) * 0x100000001B3ull;
        }
    }
    return h;
}

/* -- sending ------------------------------------------------------------------------------------------ */

static void SendServer(const char *fmt, ...)
{
    char text[256];
    int n;
    va_list args;
    if (O.sock == INVALID_SOCKET || O.sock == 0 || O.serverless) {
        return;
    }
    n = snprintf(text, sizeof(text), "BBMM ");
    va_start(args, fmt);
    n += vsnprintf(text + n, sizeof(text) - (size_t)n, fmt, args);
    va_end(args);
    sendto(O.sock, text, n, 0, (const struct sockaddr *)&O.server, sizeof(O.server));
}

static void SendDirect(const void *data, int size)
{
    sendto(O.sock, (const char *)data, size, 0, (const struct sockaddr *)&O.peer, sizeof(O.peer));
}

static void SendRelay(const void *data, int size)
{
    char packet[600];
    if (size > (int)sizeof(packet) - 8 || O.serverless) {
        return;
    }
    memcpy(packet, "BBRL", 4);
    memcpy(packet + 4, &O.matchId, 4);
    memcpy(packet + 8, data, (size_t)size);
    sendto(O.sock, packet, size + 8, 0, (const struct sockaddr *)&O.server, sizeof(O.server));
}

/* To the other machine by the path that works: direct once it has answered, else the relay. */
static void SendPeer(const void *data, int size)
{
    if (O.direct) {
        SendDirect(data, size);
    } else {
        SendRelay(data, size);
    }
}

/* A handshake message, both ways while the direct path is unproven. */
static void SendPeerText(const char *fmt, ...)
{
    char text[256];
    int n;
    va_list args;
    n = snprintf(text, sizeof(text), "BBPP %lu ", (unsigned long)O.matchId);
    va_start(args, fmt);
    n += vsnprintf(text + n, sizeof(text) - (size_t)n, fmt, args);
    va_end(args);
    SendDirect(text, n);
    if (!O.direct) {
        SendRelay(text, n);
    }
}

/* netplay.c's transport */
static void TransportSend(const void *data, int size)
{
    SendPeer(data, size);
}

static int TransportReceive(void *buffer, int capacity)
{
    int size;
    if (O.queueRead == O.queueWrite) {
        return 0;
    }
    size = O.queue[O.queueRead].size < capacity ? O.queue[O.queueRead].size : capacity;
    memcpy(buffer, O.queue[O.queueRead].data, (size_t)size);
    O.queueRead = (O.queueRead + 1) % 64;
    return size;
}

/* -- the offline game and the starting state ---------------------------------------------------------- */

static void PutOfflineAside(void)
{
    if (!O.offline) {
        O.offline = SDL_malloc(SaveStateCapacity());
    }
    if (!O.offlineSaved) {
        SaveStateSave(O.offline);
        O.offlineSaved = 1;
    }
}

static void BringOfflineBack(void)
{
    if (O.offlineSaved) {
        SaveStateLoad(O.offline);
        O.offlineSaved = 0;
    }
    HostSetSimulationMargin(HostOwnMargin());
    g_pcTicksPerFrame = HostOwnTicksPerFrame();
    NetplaySetCardsOff(0);
    HostResetPacing();
}

/* Runs the game one field with these controllers, unseen and unheard. */
static void RunFieldQuietly(unsigned pad1, unsigned pad2)
{
    int frames;
    HostWritePadPort(0, 1, pad1, NULL);
    HostWritePadPort(1, 1, pad2, NULL);
    do {
        ContextSwitchToGame();
    } while (!KernelLastYieldWasBlank());
    SpuTakeFieldAudio(&frames);
}

/* Starts preparing the state every match starts from: power-on, then the title, the main menu, VS mode and
 * its character select, by inputs chosen from the game's state alone, so every machine gets the same. */
static void StartPreparing(void)
{
    if (O.start || O.preparing) {
        return;
    }
    PutOfflineAside();
    SaveStateLoad(HostBootState());
    HostSetSimulationMargin(ONLINE_MARGIN);
    NetplaySetCardsOff(1);
    O.preparing = 1;
    O.phase = 0;
    O.phaseField = 0;
}

/* Some fields of the preparation; returns 1 when the state is ready. */
static int ContinuePreparing(void)
{
    extern short gGameState;
    int i;
    for (i = 0; i < PREPARE_FIELDS_PER_CALL; i++) {
        int field = O.preparing - 1;
        unsigned pad = 0;
        switch (O.phase) {
        case 0: /* through the boot and the title: start and cross pressed now and then */
            if (gGameState == GS_UPDATE_MAIN_MENU) {
                O.phase = 1;
                O.phaseField = field;
            } else if (field % 20 < 4) {
                pad = (field / 20) % 2 ? PAD_CROSS : PAD_START;
            }
            break;
        case 1: /* the main menu: down to VS, then cross */
            if (field - O.phaseField == 40) {
                pad = PAD_DOWN;
            } else if (field - O.phaseField >= 70 && (field - O.phaseField) % 20 < 4) {
                pad = PAD_CROSS;
            }
            if (gGameState == GS_UPDATE_CHARACTER_SELECT) {
                O.phase = 2;
                O.phaseField = field;
            }
            if (field - O.phaseField > 600) {
                O.phase = 0; /* left the menu somehow: from the title again */
            }
            break;
        case 2: /* the character select, settled */
            if (field - O.phaseField >= 60) {
                O.start = SDL_malloc(SaveStateCapacity());
                SaveStateSave(O.start);
                O.startHash = SaveStateHashState(O.start);
                O.preparing = 0;
                fprintf(stderr, "online: %u ms: the starting state is ready (%d fields, hash %016llx)\n",
                        (unsigned)SDL_GetTicks(), field, (unsigned long long)O.startHash);
                BringOfflineBack();
                return 1;
            }
            break;
        }
        if (field > 20000) {
            O.preparing = 0;
            BringOfflineBack();
            Message("Could not prepare online play: the game did not reach the character select.");
            return 1;
        }
        RunFieldQuietly(pad, 0);
        O.preparing++;
    }
    return 0;
}

/* -- the server ----------------------------------------------------------------------------------------- */

static int ParseAddress(const char *text, struct sockaddr_in *out)
{
    char host[128];
    const char *colon = strrchr(text, ':');
    struct addrinfo hints, *result;
    if (!colon || colon - text >= (int)sizeof(host)) {
        return 0;
    }
    memcpy(host, text, (size_t)(colon - text));
    host[colon - text] = 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, colon + 1, &hints, &result) != 0) {
        return 0;
    }
    memcpy(out, result->ai_addr, sizeof(*out));
    freeaddrinfo(result);
    return 1;
}

/* The socket, bound to `port` (0: any); returns 0 if that port is taken. */
static int OpenSocket(int port)
{
    struct sockaddr_in local;
    u_long nonBlocking = 1;
    if (!O.wsa) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        O.wsa = 1;
        O.sock = INVALID_SOCKET;
    }
    if (O.sock != INVALID_SOCKET && (port == 0 || port == O.socketPort)) {
        return 1;
    }
    if (O.sock != INVALID_SOCKET) {
        closesocket(O.sock);
        O.sock = INVALID_SOCKET;
    }
    O.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons((u_short)port);
    if (bind(O.sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
        closesocket(O.sock);
        O.sock = INVALID_SOCKET;
        return 0;
    }
    O.socketPort = port;
    ioctlsocket(O.sock, FIONBIO, &nonBlocking);
    {
        /* A send to a player who has gone would otherwise turn up as an error on a later receive. */
        DWORD off = 0, bytes = 0;
        WSAIoctl(O.sock, _WSAIOW(IOC_VENDOR, 12), &off, sizeof(off), NULL, 0, &bytes, NULL, NULL);
    }
    return 1;
}

void OnlineConnect(void)
{
    const char *server = SDL_getenv("DOJO_SERVER");
    if (O.state != ONLINE_OFF) {
        return;
    }
    if (!server || !*server) {
        server = g_settings.server;
    }
    O.serverless = O.hosting = 0;
    OpenSocket(0);
    if (!ParseAddress(server, &O.server)) {
        char text[160];
        snprintf(text, sizeof(text), "Cannot find the server %s.", server);
        Message(text);
        return;
    }
    O.state = ONLINE_CONNECTING;
    O.connectStartMs = O.lastPingMs = NowMs();
    O.lastServerMs = NowMs();
    SendServer("HELLO %d %016llx %s", PROTOCOL, (unsigned long long)DiscIdentity(), g_settings.name);
    /* The starting state is prepared now, while the player chooses. */
    StartPreparing();
}

void OnlineDisconnect(void)
{
    if (O.state == ONLINE_OFF) {
        return;
    }
    OnlineLeave();
    SendServer("BYE");
    O.state = ONLINE_OFF;
}

/* Where a session goes when a match ends or a wait is left. */
static OnlineState AfterMatch(void)
{
    if (O.serverless) {
        O.serverless = O.hosting = 0;
        return ONLINE_OFF;
    }
    return O.mode == ONLINE_KOTH ? ONLINE_WAITING : ONLINE_IDLE;
}

/* This machine's addresses on its local networks, for the player hosting to tell the other. */
static void LocalAddresses(char *out, size_t size, int port)
{
    char host[256];
    struct addrinfo hints, *result, *a;
    size_t n = 0;
    out[0] = 0;
    if (gethostname(host, sizeof(host)) != 0) {
        return;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &result) != 0) {
        return;
    }
    for (a = result; a && n + 24 < size; a = a->ai_next) {
        char ip[32];
        inet_ntop(AF_INET, &((struct sockaddr_in *)a->ai_addr)->sin_addr, ip, sizeof(ip));
        if (strncmp(ip, "127.", 4) != 0) {
            n += (size_t)snprintf(out + n, size - n, "%s%s:%d", n ? ", " : "", ip, port);
        }
    }
    freeaddrinfo(result);
}

static void BeginSetupAddress(uint32_t matchId, int role, const struct sockaddr_in *peer, const char *name);

void OnlineHostDirect(void)
{
    char addresses[160];
    int port = g_settings.p2pPort;
    if (O.state == ONLINE_SETUP || O.state == ONLINE_PLAYING) {
        return;
    }
    OnlineDisconnect();
    if (!OpenSocket(port)) {
        char text[120];
        snprintf(text, sizeof(text), "UDP port %d is in use: set p2p_port in dojo.ini to another.", port);
        Message(text);
        return;
    }
    O.serverless = O.hosting = 1;
    O.mode = ONLINE_PRIVATE;
    O.matchId = 0;
    O.state = ONLINE_WAITING;
    LocalAddresses(addresses, sizeof(addresses), port);
    snprintf(O.waitText, sizeof(O.waitText),
             "Hosting on UDP port %d. Your friend joins %s (same network), or your internet address with port %d "
             "forwarded on your router.%s%s",
             port, addresses[0] ? addresses : "this PC's address", port, O.publicAddress[0] ? " Seen by the server as " : "",
             O.publicAddress[0] ? O.publicAddress : "");
    StartPreparing();
}

void OnlineJoinDirect(const char *address)
{
    char text[160];
    struct sockaddr_in peer;
    if (O.state == ONLINE_SETUP || O.state == ONLINE_PLAYING) {
        return;
    }
    OnlineDisconnect();
    snprintf(text, sizeof(text), "%s%s", address, strchr(address, ':') ? "" : ":");
    if (!strchr(address, ':')) {
        snprintf(text, sizeof(text), "%s:%d", address, g_settings.p2pPort);
    }
    if (!OpenSocket(0) || !ParseAddress(text, &peer)) {
        char message[200];
        snprintf(message, sizeof(message), "Cannot find %s.", text);
        Message(message);
        return;
    }
    O.serverless = 1;
    O.hosting = 0;
    O.mode = ONLINE_PRIVATE;
    StartPreparing();
    BeginSetupAddress((uint32_t)SDL_rand_bits() | 1u, 1, &peer, "host");
}

void OnlineQuickPlay(void)
{
    if (O.state != ONLINE_IDLE) {
        return;
    }
    O.mode = ONLINE_QUICK;
    O.state = ONLINE_WAITING;
    snprintf(O.waitText, sizeof(O.waitText), "Looking for an opponent...");
    SendServer("QUICK");
}

void OnlineCreateRoom(void)
{
    if (O.state != ONLINE_IDLE) {
        return;
    }
    O.mode = ONLINE_PRIVATE;
    O.state = ONLINE_WAITING;
    O.room[0] = 0;
    snprintf(O.waitText, sizeof(O.waitText), "Creating a room...");
    SendServer("CREATE");
}

void OnlineJoinRoom(const char *code)
{
    if (O.state != ONLINE_IDLE) {
        return;
    }
    O.mode = ONLINE_PRIVATE;
    O.state = ONLINE_WAITING;
    snprintf(O.room, sizeof(O.room), "%s", code);
    snprintf(O.waitText, sizeof(O.waitText), "Joining room %s...", code);
    SendServer("JOIN %s", code);
}

void OnlineKingOfTheHill(const char *code)
{
    if (O.state != ONLINE_IDLE) {
        return;
    }
    O.mode = ONLINE_KOTH;
    O.state = ONLINE_WAITING;
    snprintf(O.room, sizeof(O.room), "%s", code && *code ? code : "");
    snprintf(O.waitText, sizeof(O.waitText), "Joining the hill...");
    O.queuePosition = O.queueLength = 0;
    O.king[0] = 0;
    SendServer("KOTH %s", code && *code ? code : "-");
}

static void StopWatching(void);
static void TakeLog(const SpectateHeader *h, int size);

static void EndMatch(const char *why)
{
    if (O.state == ONLINE_PLAYING) {
        NetplayEnd();
        BringOfflineBack();
    }
    O.matchId = 0;
    if (why) {
        Message(why);
    }
}

void OnlineLeave(void)
{
    StopWatching();
    switch (O.state) {
    case ONLINE_WAITING:
        SendServer("LEAVE");
        O.state = O.serverless ? AfterMatch() : ONLINE_IDLE;
        break;
    case ONLINE_SETUP:
    case ONLINE_PLAYING: {
        int i;
        for (i = 0; i < 3; i++) {
            SendPeerText("BYE");
        }
        SendServer("LEAVE");
        EndMatch(O.state == ONLINE_PLAYING ? "You left the match." : NULL);
        O.state = O.serverless ? AfterMatch() : ONLINE_IDLE;
        break;
    }
    default:
        break;
    }
}

/* A match the server made: reach the other machine. */
static void BeginSetup(uint32_t matchId, int role, const char *ip, int port, const char *name)
{
    struct sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port = htons((u_short)port);
    inet_pton(AF_INET, ip, &peer.sin_addr);
    BeginSetupAddress(matchId, role, &peer, name);
}

static void BeginSetupAddress(uint32_t matchId, int role, const struct sockaddr_in *peer, const char *name)
{
    O.matchId = matchId;
    O.role = role;
    O.peer = *peer;
    snprintf(O.peerName, sizeof(O.peerName), "%s", name);
    O.direct = 0;
    O.haveHello = O.sentReady = O.peerReady = 0;
    O.setupStartMs = NowMs();
    O.lastPunchMs = 0;
    O.winnerSide = -1;
    O.resultSent = 0;
    O.queueRead = O.queueWrite = 0;
    O.state = ONLINE_SETUP;
}

static void ServerLine(char *line)
{
    char word[24];
    int n = 0;
    if (strncmp(line, "PONG", 4) != 0) {
        fprintf(stderr, "online: %u ms: server: %s\n", (unsigned)SDL_GetTicks(), line);
    }
    if (sscanf(line, "%23s%n", word, &n) != 1) {
        return;
    }
    line += n;
    while (*line == ' ') {
        line++;
    }
    if (strcmp(word, "WELCOME") == 0) {
        sscanf(line, "%63s", O.publicAddress);
        if (O.state == ONLINE_CONNECTING) {
            O.state = ONLINE_IDLE;
        }
    } else if (strcmp(word, "PONG") == 0) {
    } else if (strcmp(word, "ERROR") == 0) {
        Message(line);
        if (O.state == ONLINE_CONNECTING) {
            O.state = ONLINE_OFF;
        } else if (O.state == ONLINE_WAITING) {
            O.state = ONLINE_IDLE;
        }
    } else if (strcmp(word, "ROOM") == 0) {
        sscanf(line, "%15s", O.room);
        snprintf(O.waitText, sizeof(O.waitText), "Room %s: waiting for your friend to join with this code.", O.room);
    } else if (strcmp(word, "QUEUED") == 0) {
        int players = 0;
        sscanf(line, "%d", &players);
        snprintf(O.waitText, sizeof(O.waitText), "Looking for an opponent... (%d online)", players);
    } else if (strcmp(word, "HILL") == 0) {
        /* HILL code position queueLength streak king */
        char code[16], king[40];
        int position = 0, length = 0, streak = 0;
        king[0] = 0;
        if (sscanf(line, "%15s %d %d %d %39s", code, &position, &length, &streak, king) >= 4) {
            O.queuePosition = position;
            O.queueLength = length;
            O.kingStreak = streak;
            snprintf(O.king, sizeof(O.king), "%s", king[0] && strcmp(king, "-") ? king : "nobody");
            if (strcmp(code, "-") != 0) {
                snprintf(O.room, sizeof(O.room), "%s", code);
            }
            if (position == 0) {
                snprintf(O.waitText, sizeof(O.waitText), "You are on the hill (%d win%s). Waiting for a challenger.",
                         streak, streak == 1 ? "" : "s");
            } else {
                snprintf(O.waitText, sizeof(O.waitText), "King: %s (%d win%s). You are number %d of %d in line.",
                         O.king, streak, streak == 1 ? "" : "s", position, length);
            }
        }
    } else if (strcmp(word, "MATCH") == 0) {
        /* MATCH id role ip port name */
        unsigned long id;
        int role, port;
        char ip[64], name[40];
        if (sscanf(line, "%lu %d %63s %d %39s", &id, &role, ip, &port, name) == 5 && (uint32_t)id != O.matchId) {
            if (O.state == ONLINE_PLAYING) {
                /* King of the Hill: the other player's result ended the match first; the next is ready. */
                int won = O.winnerSide == O.role;
                EndMatch(O.winnerSide < 0 ? NULL
                         : won ? "You won! You stay on the hill." : "You lost. Back in line for another go.");
            }
            BeginSetup((uint32_t)id, role, ip, port, name);
        }
    } else if (strcmp(word, "SPECTATE") == 0) {
        unsigned long id;
        char king[40], challenger[40];
        if (sscanf(line, "%lu %39s %39s", &id, king, challenger) == 3 && O.mode == ONLINE_KOTH &&
            O.state == ONLINE_WAITING && (uint32_t)id != O.W.matchId) {
            StopWatching();
            O.W.on = 1;
            O.W.matchId = (uint32_t)id;
            snprintf(O.W.names[0], sizeof(O.W.names[0]), "%s", king);
            snprintf(O.W.names[1], sizeof(O.W.names[1]), "%s", challenger);
            O.W.lastNewMs = NowMs();
        }
    } else if (strcmp(word, "SPECEND") == 0) {
        unsigned long id, total;
        if (sscanf(line, "%lu %lu", &id, &total) == 2 && (uint32_t)id == O.W.matchId) {
            O.W.ended = 1;
            O.W.total = (uint32_t)total;
        }
    } else if (strcmp(word, "SPACK") == 0) {
        unsigned long id, have;
        if (sscanf(line, "%lu %lu", &id, &have) == 2 && (uint32_t)id == O.matchId && (uint32_t)have > O.pushAcked) {
            O.pushAcked = (uint32_t)have;
        }
    } else if (strcmp(word, "PEERLEFT") == 0) {
        unsigned long id = 0;
        sscanf(line, "%lu", &id);
        if ((uint32_t)id == O.matchId && (O.state == ONLINE_SETUP || O.state == ONLINE_PLAYING)) {
            EndMatch("Your opponent left.");
            O.state = AfterMatch();
        }
    }
}

/* A handshake message from the other machine. */
static void PeerLine(char *line, int direct)
{
    unsigned long id;
    char word[16];
    int n = 0;
    if (sscanf(line, "%lu %15s%n", &id, word, &n) != 2 || (uint32_t)id != O.matchId || !O.matchId) {
        return;
    }
    line += n;
    if (direct && strcmp(word, "PUNCH") == 0) {
        SendPeerText("PUNCHED");
    } else if (direct && strcmp(word, "PUNCHED") == 0 && !SDL_getenv("DOJO_NO_DIRECT")) {
        /* (DOJO_NO_DIRECT: never trust the direct path, for testing the relay) */
        if (!O.direct) {
            fprintf(stderr, "online: connected directly\n");
        }
        O.direct = 1;
    } else if (strcmp(word, "HELLO") == 0) {
        unsigned long long hash;
        char name[40];
        int fields = sscanf(line, "%d %d %llx %39s", &O.peerTicks, &O.peerDelay, &hash, name);
        if (fields >= 3) {
            O.peerHash = hash;
            O.haveHello = 1;
            if (fields == 4) {
                snprintf(O.peerName, sizeof(O.peerName), "%s", name);
            }
        }
    } else if (strcmp(word, "READY") == 0) {
        O.peerReady = 1;
    } else if (strcmp(word, "BYE") == 0) {
        if (O.state == ONLINE_SETUP || O.state == ONLINE_PLAYING) {
            EndMatch("Your opponent left.");
            O.state = AfterMatch();
        }
    }
}

static void ReceiveAll(void)
{
    char buffer[700];
    struct sockaddr_in from;
    int fromLen, got;
    for (;;) {
        char *data = buffer;
        int fromServer, direct;
        fromLen = sizeof(from);
        got = recvfrom(O.sock, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *)&from, &fromLen);
        if (got <= 0) {
            break;
        }
        fromServer = from.sin_addr.s_addr == O.server.sin_addr.s_addr && from.sin_port == O.server.sin_port;
        direct = !fromServer;
        if (fromServer) {
            O.lastServerMs = NowMs();
        }
        if (fromServer && got >= 8 && memcmp(buffer, "BBRL", 4) == 0) {
            uint32_t id;
            memcpy(&id, buffer + 4, 4);
            if (id != O.matchId) {
                continue;
            }
            data += 8;
            got -= 8;
        } else if (fromServer && got >= 5 && memcmp(buffer, "BBMM ", 5) == 0) {
            buffer[got] = 0;
            ServerLine(buffer + 5);
            continue;
        } else if (fromServer && got >= (int)sizeof(SpectateHeader) && memcmp(buffer, "BBSP", 4) == 0) {
            TakeLog((const SpectateHeader *)buffer, got);
            continue;
        } else if (!fromServer && O.hosting && O.state == ONLINE_WAITING && got >= 5 &&
                   memcmp(buffer, "BBPP ", 5) == 0) {
            /* Hosting player to player: whoever knocks first is the opponent, and their match id is the match's. */
            unsigned long id = strtoul(buffer + 5, NULL, 10);
            if (id == 0) {
                continue;
            }
            BeginSetupAddress((uint32_t)id, 0, &from, "guest");
        } else if (!fromServer && (from.sin_addr.s_addr != O.peer.sin_addr.s_addr || from.sin_port != O.peer.sin_port)) {
            continue; /* a stranger */
        }
        if (got >= 5 && memcmp(data, "BBPP ", 5) == 0) {
            data[got] = 0;
            PeerLine(data + 5, direct);
        } else if (NetplayIsSessionPacket(data, got) && O.state == ONLINE_PLAYING &&
                   got <= (int)sizeof(O.queue[0].data) && (O.queueWrite + 1) % 64 != O.queueRead) {
            memcpy(O.queue[O.queueWrite].data, data, (size_t)got);
            O.queue[O.queueWrite].size = got;
            O.queueWrite = (O.queueWrite + 1) % 64;
        }
    }
}

/* -- the match ------------------------------------------------------------------------------------------ */

static void StartMatch(void)
{
    static const NetplayTransport transport = {TransportSend, TransportReceive};
    StopWatching();
    O.pushAcked = 0;
    O.lastPushMs = 0;
    PutOfflineAside();
    SaveStateLoad(O.start);
    HostSetSimulationMargin(ONLINE_MARGIN);
    g_pcTicksPerFrame = O.ticks;
    HostResetPacing();
    NetplayBegin(&transport, O.role, O.delay);
    O.state = ONLINE_PLAYING;
    O.matchStartMs = NowMs();
    fprintf(stderr, "online: match %lu against %s, %s, %d ticks a frame\n", (unsigned long)O.matchId, O.peerName,
            O.direct ? "direct" : "relayed", O.ticks);
}

static void UpdateSetup(void)
{
    uint32_t now = NowMs();
    if (now - O.lastPunchMs >= PUNCH_MS) {
        O.lastPunchMs = now;
        SendPeerText("PUNCH");
        if (O.start) {
            SendPeerText("HELLO %d %d %016llx %s", HostOwnTicksPerFrame(), ONLINE_DELAY, (unsigned long long)O.startHash,
                         g_settings.name);
        }
        if (O.sentReady) {
            SendPeerText("READY");
        }
    }
    if (!O.start) {
        return; /* still preparing */
    }
    if (O.haveHello && !O.sentReady) {
        if (O.peerHash != O.startHash) {
            SendPeerText("BYE");
            SendServer("LEAVE");
            O.matchId = 0;
            O.state = O.serverless ? AfterMatch() : ONLINE_IDLE;
            Message("Your opponent's game differs from yours (another version of the port or disc).");
            return;
        }
        O.ticks = O.peerTicks > HostOwnTicksPerFrame() ? O.peerTicks : HostOwnTicksPerFrame();
        O.delay = O.peerDelay > ONLINE_DELAY ? O.peerDelay : ONLINE_DELAY;
        O.sentReady = 1;
        SendPeerText("READY");
    }
    /* Direct once proven, else the relay after a moment (player to player there is none); both go on with
     * whichever they have. */
    if (O.sentReady && O.peerReady && (O.direct || (!O.serverless && now - O.setupStartMs >= RELAY_AFTER_MS))) {
        StartMatch();
        return;
    }
    if (now - O.setupStartMs > SETUP_GIVE_UP_MS) {
        int serverless = O.serverless;
        SendServer("LEAVE");
        O.matchId = 0;
        O.state = serverless ? AfterMatch() : ONLINE_IDLE;
        Message(serverless ? "Could not reach the host. Over the internet the host's router must forward the UDP port."
                           : "Could not reach your opponent.");
    }
}

/* A hill match's player: the settled inputs the server lacks, for those watching. */
static void PushLog(void)
{
    char packet[sizeof(SpectateHeader) + SPECTATE_BATCH * 4];
    SpectateHeader *h = (SpectateHeader *)packet;
    uint32_t *words = (uint32_t *)(packet + sizeof(SpectateHeader));
    uint32_t settled = NetplaySettledFields(), now = NowMs(), f;
    uint16_t count = 0;
    if (now - O.lastPushMs < PUSH_MS || O.pushAcked >= settled) {
        return;
    }
    O.lastPushMs = now;
    for (f = O.pushAcked; f < settled && count < SPECTATE_BATCH; f++) {
        uint16_t a, b;
        if (!NetplayInputsAt(f, &a, &b)) {
            break; /* fallen out of the ring: those watching will stop where it ends */
        }
        words[count++] = a | (uint32_t)b << 16;
    }
    if (!count) {
        return;
    }
    memcpy(h->magic, "BBSP", 4);
    h->matchId = O.matchId;
    h->first = O.pushAcked;
    h->count = count;
    h->ticks = (uint16_t)O.ticks;
    h->total = 0;
    sendto(O.sock, packet, (int)(sizeof(SpectateHeader) + count * 4u), 0, (const struct sockaddr *)&O.server,
           sizeof(O.server));
}

static void UpdateMatch(void)
{
    int gameState, winner;
    if (NetplaySilentMs() > SILENCE_MS) {
        SendServer("LEAVE");
        EndMatch("The connection to your opponent was lost.");
        O.state = AfterMatch();
        return;
    }
    if (O.mode == ONLINE_KOTH) {
        PushLog();
    }
    if (!NetplaySettled(&gameState, &winner)) {
        return;
    }
    if (winner >= 0 && gameState >= GS_INIT_BATTLE_REPLAY) {
        O.winnerSide = winner;
    }
    /* King of the Hill: a bout decides it; once the game reaches its continue screen, the match is over. */
    if (O.mode == ONLINE_KOTH && O.winnerSide >= 0 &&
        (gameState == GS_INIT_CONTINUE_SCREEN || gameState == GS_INIT_MAIN_MENU)) {
        int won = O.winnerSide == O.role;
        SendServer("RESULT %lu %d", (unsigned long)O.matchId, O.winnerSide);
        EndMatch(won ? "You won! You stay on the hill." : "You lost. Back in line for another go.");
        O.state = ONLINE_WAITING;
    }
}

int OnlineUpdate(void)
{
    uint32_t now = NowMs();
    if (O.state == ONLINE_OFF && !O.preparing) {
        return 0;
    }
    if (O.sock != INVALID_SOCKET && O.sock != 0) {
        ReceiveAll();
        if (now - O.lastPingMs >= PING_MS && O.state != ONLINE_OFF && !O.serverless) {
            O.lastPingMs = now;
            if (O.state == ONLINE_CONNECTING) {
                SendServer("HELLO %d %016llx %s", PROTOCOL, (unsigned long long)DiscIdentity(), g_settings.name);
            } else {
                SendServer("PING");
            }
        }
        if (O.state != ONLINE_OFF && !O.serverless && (int32_t)(now - O.lastServerMs) > SERVER_SILENCE_MS &&
            O.state != ONLINE_PLAYING) {
            int wasConnecting = O.state == ONLINE_CONNECTING;
            OnlineLeave();
            O.state = ONLINE_OFF;
            Message(wasConnecting ? "The server does not answer." : "Lost the connection to the server.");
        }
    }
    if (O.preparing) {
        ContinuePreparing();
        return O.preparing != 0;
    }
    switch (O.state) {
    case ONLINE_SETUP:
        UpdateSetup();
        break;
    case ONLINE_PLAYING:
        UpdateMatch();
        break;
    default:
        break;
    }
    return 0;
}

/* -- watching ------------------------------------------------------------------------------------------ */

static void TakeLog(const SpectateHeader *h, int size)
{
    const uint32_t *words = (const uint32_t *)(h + 1);
    uint32_t i;
    if (!O.W.on || h->matchId != O.W.matchId || size < (int)(sizeof(*h) + h->count * 4u)) {
        return;
    }
    O.W.ticks = h->ticks ? h->ticks : 1;
    if (h->total > O.W.total) {
        O.W.total = h->total;
    }
    if (h->first > O.W.have || h->first + h->count <= O.W.have) {
        return; /* a gap, or nothing new: asked again */
    }
    if (h->first + h->count > O.W.capacity) {
        O.W.capacity = (h->first + h->count) * 2 + 1024;
        O.W.log = SDL_realloc(O.W.log, O.W.capacity * sizeof(uint32_t));
    }
    for (i = O.W.have - h->first; i < h->count; i++) {
        O.W.log[h->first + i] = words[i];
    }
    O.W.have = h->first + h->count;
    O.W.lastNewMs = NowMs();
}

static void StopWatching(void)
{
    if (O.W.running) {
        BringOfflineBack();
    }
    O.W.on = O.W.running = O.W.ended = 0;
    O.W.matchId = 0;
    O.W.have = O.W.total = O.W.next = 0;
}

void OnlineStopWatching(void)
{
    /* The log stays: turned on again, the same fight is caught up with. */
    if (O.W.running) {
        BringOfflineBack();
        O.W.running = 0;
        O.W.next = 0;
    }
    g_settings.watch = 0;
}

int OnlineWatching(void)
{
    return O.W.running;
}

int OnlineWantsToWatch(void)
{
    return O.W.on && g_settings.watch && O.start && O.state == ONLINE_WAITING;
}

/* One field of the watched fight, with its ticks shown and its sound played, unless `quietly`. */
static void RunWatchedField(int quietly)
{
    uint32_t pads = O.W.log[O.W.next];
    const int16_t *samples;
    int frames;
    if (O.W.next % 30 == 0 && O.W.next && SDL_getenv("DOJO_HASH_EVERY")) {
        /* the hash the players' netplay logs for the same field: they must agree */
        fprintf(stderr, "watch: match %lu field %lu hash %016llx\n", (unsigned long)O.W.matchId,
                (unsigned long)O.W.next, (unsigned long long)SaveStateHash());
    }
    HostWritePadPort(0, 1, pads & 0xFFFF, NULL);
    HostWritePadPort(1, 1, pads >> 16, NULL);
    for (;;) {
        ContextSwitchToGame();
        if (KernelLastYieldWasBlank()) {
            break;
        }
        if (!quietly) {
            HostPresentTick();
        }
    }
    samples = SpuTakeFieldAudio(&frames);
    if (!quietly && frames) {
        HostQueueAudio(samples, frames);
    }
    O.W.next++;
}

int OnlineWatchField(void)
{
    uint32_t now = NowMs();
    if (!OnlineWantsToWatch()) {
        return 0;
    }
    /* Ask for what is missing, a few times a second. */
    if (now - O.W.lastAskMs >= WATCH_MS) {
        O.W.lastAskMs = now;
        SendServer("WATCH %lu %lu", (unsigned long)O.W.matchId, (unsigned long)O.W.have);
    }
    if (!O.W.running) {
        if (!O.start || O.W.have == 0) {
            return 0;
        }
        /* The fight's first inputs are in: from the starting state, at the match's rate. */
        PutOfflineAside();
        SaveStateLoad(O.start);
        HostSetSimulationMargin(ONLINE_MARGIN);
        NetplaySetCardsOff(1);
        g_pcTicksPerFrame = O.W.ticks;
        HostResetPacing();
        O.W.running = 1;
        O.W.next = 0;
    }
    /* Far behind (joined late): run on unseen to near the newest. */
    if (O.W.have > O.W.next + WATCH_CATCH_UP) {
        while (O.W.next + WATCH_BEHIND < O.W.have) {
            RunWatchedField(1);
        }
        HostResetPacing();
    }
    if (O.W.next < O.W.have && (O.W.ended || O.W.next + WATCH_BEHIND / 2 < O.W.have)) {
        RunWatchedField(0);
        return 1;
    }
    if ((O.W.ended && O.W.next >= O.W.total && O.W.next >= O.W.have) || now - O.W.lastNewMs > WATCH_STALL_MS) {
        StopWatching(); /* the fight is over, or its log stopped coming */
    }
    return 0;
}

OnlineState OnlineGetState(void)
{
    return O.state;
}

OnlineMode OnlineGetMode(void)
{
    return O.mode;
}

int OnlineInMatch(void)
{
    return O.state == ONLINE_PLAYING;
}

void OnlineDescribe(char *out, int size)
{
    switch (O.state) {
    case ONLINE_OFF:
        snprintf(out, (size_t)size, "Not connected.");
        break;
    case ONLINE_CONNECTING:
        snprintf(out, (size_t)size, "Connecting to the server...");
        break;
    case ONLINE_IDLE:
        snprintf(out, (size_t)size, O.preparing ? "Connected. Getting the game ready..." : "Connected. Choose how to play.");
        break;
    case ONLINE_WAITING:
        snprintf(out, (size_t)size, "%s", O.waitText);
        break;
    case ONLINE_SETUP:
        if (O.serverless && O.role == 1) {
            snprintf(out, (size_t)size, "Connecting to the host...");
        } else {
            snprintf(out, (size_t)size, "Opponent found: %s. Connecting...", O.peerName);
        }
        break;
    case ONLINE_PLAYING:
        snprintf(out, (size_t)size, "Playing %s.", O.peerName);
        break;
    }
}

void OnlineBanner(char *out, int size)
{
    out[0] = 0;
    if (O.W.running) {
        if (O.queuePosition > 0) {
            snprintf(out, (size_t)size, "Watching %s vs %s   You are %d of %d in line   (hold Back: stop watching)",
                     O.W.names[0], O.W.names[1], O.queuePosition, O.queueLength);
        } else {
            snprintf(out, (size_t)size, "Watching %s vs %s   (hold Back: stop watching)", O.W.names[0], O.W.names[1]);
        }
        return;
    }
    if (O.state == ONLINE_PLAYING) {
        const char *status = NetplayStatus();
        unsigned rtt = 0;
        if (status) {
            const char *r = strstr(status, "rtt ");
            if (r) {
                rtt = (unsigned)atoi(r + 4);
            }
        }
        if (O.mode == ONLINE_KOTH) {
            snprintf(out, (size_t)size, "vs %s   %u ms   King of the Hill", O.peerName, rtt);
        } else {
            snprintf(out, (size_t)size, "vs %s   %u ms", O.peerName, rtt);
        }
    } else if (O.state == ONLINE_WAITING || O.state == ONLINE_SETUP) {
        OnlineDescribe(out, size);
    }
}
