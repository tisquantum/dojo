/* Online play with rollback (netplay.h).
 *
 * The unit is the field, the emulated vertical blank the game yields at (kernel.c): each field the two
 * controllers are written and the game runs to the next one. The simulation is a function of those inputs
 * alone (DOJO_HASH_EVERY shows it: the same inputs give the same state on any machine), so each machine
 * runs both players: its own with its input `delay` fields late, the other's predicted as the last input it
 * has, and when the real input arrives and differs, it loads the state saved before that field and runs
 * forward again with drawing and sound left out.
 *
 * Packets are UDP and carry every input the other machine has not acknowledged, so a lost packet costs
 * nothing; each also carries the sender's newest field, for keeping the two in step, and from time to
 * time the hash of a field both have settled, for noticing a desync. */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "cdrom.h"
#include "kernel_port.h"
#include "context.h"
#include "host.h"
#include "netplay.h"
#include "savestate.h"
#include "spu.h"

#define MAGIC          0x504E4242u /* "BBNP" */
#define VERSION        3
#define RING           64          /* fields of inputs and states kept */
#define MAX_ROLLBACK   12          /* how far a machine may run on predictions */
#define MAX_SEND       60
#define HASH_INTERVAL  30

enum { PACKET_HELLO = 1, PACKET_WELCOME = 2, PACKET_INPUT = 3 };

#pragma pack(push, 1)
typedef struct Hello {
    uint32_t magic;
    uint8_t type;
    uint8_t version;
    uint16_t delay;
    uint16_t ticks;
    uint16_t pad;
    uint32_t margin;
    uint64_t disc;
} Hello;

typedef struct InputPacket {
    uint32_t magic;
    uint8_t type;
    uint8_t pad;
    uint16_t count;
    uint32_t first;       /* the field of inputs[0] */
    uint32_t ack;         /* every field before this one of the receiver's inputs has arrived */
    uint32_t newest;      /* the sender's next field to run */
    uint32_t sentMs, echoMs;
    uint32_t hashField;   /* a settled field and the hash of the state before it, or 0 */
    uint64_t hash;
    uint16_t inputs[MAX_SEND];
} InputPacket;
#pragma pack(pop)

static struct {
    int active;
    SOCKET sock;             /* DOJO_NET's own socket; online play (online.c) brings its transport */
    struct sockaddr_in peer;
    void (*send)(const void *data, int size);
    int (*receive)(void *buffer, int capacity);
    uint32_t lastHeardMs;    /* when the other machine's last packet came */
    uint32_t info[RING];     /* each field's game state and bout winner after it ran (NetplaySettled) */
    int local;               /* this machine's controller port: 0 hosting, 1 joining */
    int delay;
    uint32_t field;          /* the next field to run */
    uint16_t localInputs[RING];
    uint16_t remoteInputs[RING];
    uint16_t usedRemote[RING];
    uint32_t remoteKnown;    /* every remote input before this field has arrived */
    uint32_t remoteAcked;    /* every local input before this field has arrived there */
    uint32_t remoteNewest;
    uint32_t rollbackFrom;   /* the oldest field run on a wrong prediction, or UINT32_MAX */
    uint8_t *states[RING];
    uint32_t stateField[RING];
    uint64_t hashes[RING];
    uint32_t hashedField[RING];
    uint32_t lastHashSent;
    uint32_t peerSentMs, peerSentAt;
    uint32_t rttMs;
    unsigned rollbacks, rolledFields, stalls;
    char desync[96];
    char status[160];
    int lossPercent, lagMs;  /* DOJO_NET_LOSS, DOJO_NET_LAG: a bad network, for testing */
} N;

/* Packets held back to simulate lag. */
#define LAG_QUEUE 256
static struct {
    uint32_t due;
    int size;
    char data[sizeof(InputPacket)];
} sLagged[LAG_QUEUE];
static int sLaggedCount;

static uint32_t NowMs(void)
{
    return (uint32_t)SDL_GetTicks();
}

static void SendRaw(const void *data, int size)
{
    if (N.lossPercent && (SDL_rand(100) < N.lossPercent)) {
        return;
    }
    if (N.lagMs && sLaggedCount < LAG_QUEUE) {
        sLagged[sLaggedCount].due = NowMs() + (uint32_t)N.lagMs;
        sLagged[sLaggedCount].size = size;
        memcpy(sLagged[sLaggedCount].data, data, (size_t)size);
        sLaggedCount++;
        return;
    }
    N.send(data, size);
}

static void FlushLagged(void)
{
    int i = 0;
    while (i < sLaggedCount) {
        if ((int32_t)(NowMs() - sLagged[i].due) >= 0) {
            N.send(sLagged[i].data, sLagged[i].size);
            sLagged[i] = sLagged[--sLaggedCount];
        } else {
            i++;
        }
    }
}

/* DOJO_NET's transport: its own socket, straight to the other machine. */
static void SocketSend(const void *data, int size)
{
    sendto(N.sock, data, size, 0, (const struct sockaddr *)&N.peer, sizeof(N.peer));
}

static int SocketReceive(void *buffer, int capacity)
{
    int got = recv(N.sock, (char *)buffer, capacity, 0);
    return got > 0 ? got : 0;
}

/* The ring and counters of a session starting at field 0. */
static void BeginSession(void)
{
    int i;
    for (i = 0; i < RING; i++) {
        if (!N.states[i]) {
            N.states[i] = SDL_malloc(SaveStateCapacity());
        }
        N.stateField[i] = UINT32_MAX;
        N.hashedField[i] = UINT32_MAX;
        N.info[i] = 0;
    }
    N.field = 0;
    /* The first `delay` fields have no input from either player. */
    N.remoteKnown = (uint32_t)N.delay;
    N.remoteAcked = 0;
    N.remoteNewest = 0;
    N.rollbackFrom = UINT32_MAX;
    N.lastHashSent = 0;
    N.rollbacks = N.rolledFields = N.stalls = 0;
    N.desync[0] = 0;
    N.lastHeardMs = NowMs();
    sLaggedCount = 0;
    N.active = 1;
}

void NetplayBegin(const NetplayTransport *transport, int local, int delay)
{
    uint8_t *states[RING];
    memcpy(states, N.states, sizeof(states));
    memset(&N, 0, sizeof(N));
    memcpy(N.states, states, sizeof(states));
    N.send = transport->send;
    N.receive = transport->receive;
    N.local = local;
    N.delay = delay < 0 ? 0 : (delay > 8 ? 8 : delay);
    {
        const char *text;
        N.lossPercent = (text = SDL_getenv("DOJO_NET_LOSS")) ? atoi(text) : 0;
        N.lagMs = (text = SDL_getenv("DOJO_NET_LAG")) ? atoi(text) : 0;
    }
    BeginSession();
    fprintf(stderr, "netplay: session as player %d, input delay %d fields\n", N.local + 1, N.delay);
}

void NetplayEnd(void)
{
    N.active = 0;
}

int NetplayLocalPlayer(void)
{
    return N.local;
}

uint32_t NetplaySilentMs(void)
{
    return N.active ? NowMs() - N.lastHeardMs : 0;
}

int NetplaySettled(int *gameState, int *winnerSide)
{
    uint32_t settled = N.remoteKnown < N.field ? N.remoteKnown : N.field;
    uint32_t info;
    if (!N.active || settled == 0 || N.stateField[(settled - 1) % RING] != settled - 1) {
        return 0;
    }
    info = N.info[(settled - 1) % RING];
    *gameState = (int)(int16_t)(info & 0xFFFF);
    *winnerSide = (int)(int16_t)(info >> 16);
    return 1;
}

static uint64_t DiscIdentity(void)
{
    unsigned char sector[2352];
    uint64_t h = 0xCBF29CE484222325ull;
    int lba, i;
    /* The volume descriptor and the start of the file system. */
    for (lba = 16; lba < 40; lba++) {
        CdReadRawSector(lba, sector);
        for (i = 0; i < 2352; i++) {
            h = (h ^ sector[i]) * 0x100000001B3ull;
        }
    }
    return h;
}

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

int NetplayStart(int simMargin, int ticksPerFrame, int *agreedMargin, int *agreedTicks)
{
    const char *spec = SDL_getenv("DOJO_NET");
    const char *text;
    WSADATA wsa;
    struct sockaddr_in local;
    u_long nonBlocking = 1;
    Hello hello, reply;
    uint64_t disc;
    int port, i;

    if (!spec) {
        return 0;
    }
    memset(&N, 0, sizeof(N));
    N.delay = (text = SDL_getenv("DOJO_NET_DELAY")) ? atoi(text) : 2;
    N.lossPercent = (text = SDL_getenv("DOJO_NET_LOSS")) ? atoi(text) : 0;
    N.lagMs = (text = SDL_getenv("DOJO_NET_LAG")) ? atoi(text) : 0;
    if (N.delay < 0) N.delay = 0;
    if (N.delay > 8) N.delay = 8;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    N.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    disc = DiscIdentity();

    memset(&hello, 0, sizeof(hello));
    hello.magic = MAGIC;
    hello.version = VERSION;
    hello.delay = (uint16_t)N.delay;
    hello.margin = (uint32_t)simMargin;
    hello.ticks = (uint16_t)ticksPerFrame;
    hello.disc = disc;

    if (strncmp(spec, "host:", 5) == 0) {
        struct sockaddr_in from;
        int fromLen = sizeof(from);
        port = atoi(spec + 5);
        local.sin_port = htons((u_short)port);
        if (bind(N.sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
            HostFatal("netplay: cannot listen on UDP port %d", port);
        }
        fprintf(stderr, "netplay: waiting for a player on UDP port %d\n", port);
        for (;;) {
            int got = recvfrom(N.sock, (char *)&reply, sizeof(reply), 0, (struct sockaddr *)&from, &fromLen);
            if (got == sizeof(reply) && reply.magic == MAGIC && reply.type == PACKET_HELLO) {
                break;
            }
        }
        N.peer = from;
        N.local = 0;
    } else if (strncmp(spec, "join:", 5) == 0) {
        if (!ParseAddress(spec + 5, &N.peer)) {
            HostFatal("netplay: cannot resolve %s", spec + 5);
        }
        bind(N.sock, (struct sockaddr *)&local, sizeof(local));
        ioctlsocket(N.sock, FIONBIO, &nonBlocking);
        N.local = 1;
        fprintf(stderr, "netplay: joining %s\n", spec + 5);
        hello.type = PACKET_HELLO;
        for (i = 0;; i++) {
            int got;
            if (i % 50 == 0) {
                sendto(N.sock, (const char *)&hello, sizeof(hello), 0, (struct sockaddr *)&N.peer, sizeof(N.peer));
            }
            SDL_Delay(10);
            got = recv(N.sock, (char *)&reply, sizeof(reply), 0);
            if (got == sizeof(reply) && reply.magic == MAGIC && reply.type == PACKET_WELCOME) {
                break;
            }
            if (i > 3000) {
                HostFatal("netplay: no answer from %s", spec + 5);
            }
        }
    } else {
        HostFatal("netplay: DOJO_NET must be host:PORT or join:ADDRESS:PORT");
    }
    if (reply.version != VERSION) {
        HostFatal("netplay: the other player runs a different version of the port");
    }
    if (reply.disc != disc) {
        HostFatal("netplay: the other player has a different disc (both need Bushido Blade USA)");
    }
    if (N.local == 0) {
        hello.type = PACKET_WELCOME;
        for (i = 0; i < 5; i++) {
            sendto(N.sock, (const char *)&hello, sizeof(hello), 0, (struct sockaddr *)&N.peer, sizeof(N.peer));
        }
        ioctlsocket(N.sock, FIONBIO, &nonBlocking);
    }
    /* Both take the larger input delay and widescreen setting, so both run the same simulation. */
    N.delay = reply.delay > N.delay ? reply.delay : N.delay;
    *agreedMargin = (int)reply.margin > simMargin ? (int)reply.margin : simMargin;
    *agreedTicks = (int)reply.ticks > ticksPerFrame ? (int)reply.ticks : ticksPerFrame;
    N.send = SocketSend;
    N.receive = SocketReceive;
    BeginSession();
    fprintf(stderr, "netplay: connected as player %d, input delay %d fields\n", N.local + 1, N.delay);
    return 1;
}

int NetplayActive(void)
{
    return N.active;
}

static uint16_t RemoteInputFor(uint32_t field)
{
    /* Known, or predicted: the newest input known before it. */
    if (field < N.remoteKnown) {
        return N.remoteInputs[field % RING];
    }
    return N.remoteKnown > 0 ? N.remoteInputs[(N.remoteKnown - 1) % RING] : 0;
}

/* Runs one field: saves the state before it, writes both controllers, and lets the game run to the next
 * vertical blank, through any ticks that end before it (each shown, unless the field is run again after a
 * rollback, which is neither drawn nor heard). */
static void RunGameField(uint32_t field, int again)
{
    uint8_t *state = N.states[field % RING];
    uint16_t remote = RemoteInputFor(field);
    uint16_t local = N.localInputs[field % RING];
    const int16_t *samples;
    int frames;

    SaveStateSave(state);
    N.stateField[field % RING] = field;
    N.usedRemote[field % RING] = remote;
    HostWritePadPort(N.local, 1, local, NULL);
    HostWritePadPort(1 - N.local, 1, remote, NULL);
    for (;;) {
        ContextSwitchToGame();
        if (KernelLastYieldWasBlank()) {
            break;
        }
        if (!again) {
            HostPresentTick();
        }
    }
    {
        extern short gGameState;
        extern short gMatchWinnerSide;
        N.info[field % RING] = (uint16_t)gGameState | (uint32_t)(uint16_t)gMatchWinnerSide << 16;
    }
    samples = SpuTakeFieldAudio(&frames);
    if (!again && frames) {
        HostQueueAudio(samples, frames);
    }
}

static void Receive(void)
{
    InputPacket p;
    int got;
    while ((got = N.receive(&p, sizeof(p))) > 0) {
        uint32_t i;
        if (got < (int)offsetof(InputPacket, inputs) || p.magic != MAGIC || p.type != PACKET_INPUT) {
            continue;
        }
        N.lastHeardMs = NowMs();
        for (i = 0; i < p.count && i < MAX_SEND; i++) {
            uint32_t field = p.first + i;
            if (field < N.remoteKnown || field != N.remoteKnown) {
                continue; /* had it already, or a gap: inputs are taken in order */
            }
            N.remoteInputs[field % RING] = p.inputs[i];
            N.remoteKnown = field + 1;
            /* A field already run on a prediction that was wrong: run again from there. */
            if (field < N.field && N.stateField[field % RING] == field && N.usedRemote[field % RING] != p.inputs[i] &&
                field < N.rollbackFrom) {
                N.rollbackFrom = field;
            }
        }
        if (p.ack > N.remoteAcked) {
            N.remoteAcked = p.ack;
        }
        if (p.newest > N.remoteNewest) {
            N.remoteNewest = p.newest;
        }
        N.peerSentMs = p.sentMs;
        N.peerSentAt = NowMs();
        if (p.echoMs) {
            N.rttMs = NowMs() - p.echoMs;
        }
        if (p.hashField && N.hashedField[p.hashField % RING] == p.hashField &&
            N.hashes[p.hashField % RING] != p.hash && !N.desync[0]) {
            snprintf(N.desync, sizeof(N.desync), "DESYNC at field %lu", (unsigned long)p.hashField);
            fprintf(stderr, "netplay: %s\n", N.desync);
        }
    }
}

static void Send(void)
{
    InputPacket p;
    uint32_t newestLocal = N.field + (uint32_t)N.delay; /* local inputs exist up to here */
    uint32_t first = N.remoteAcked, count;
    memset(&p, 0, sizeof(p));
    if (newestLocal - first > MAX_SEND) {
        first = newestLocal - MAX_SEND;
    }
    count = newestLocal - first;
    p.magic = MAGIC;
    p.type = PACKET_INPUT;
    p.count = (uint16_t)count;
    p.first = first;
    p.ack = N.remoteKnown;
    p.newest = N.field;
    p.sentMs = NowMs();
    p.echoMs = N.peerSentMs ? N.peerSentMs + (NowMs() - N.peerSentAt) : 0;
    {
        uint32_t i;
        for (i = 0; i < count; i++) {
            p.inputs[i] = N.localInputs[(first + i) % RING];
        }
    }
    /* The hash of the newest settled field on the interval: both machines have every input before it. */
    {
        uint32_t settled = N.remoteKnown < N.field ? N.remoteKnown : N.field;
        uint32_t h = settled / HASH_INTERVAL * HASH_INTERVAL;
        if (h > 0 && h > N.lastHashSent && N.stateField[h % RING] == h && N.rollbackFrom > h) {
            N.hashes[h % RING] = SaveStateHashState(N.states[h % RING]);
            N.hashedField[h % RING] = h;
            N.lastHashSent = h;
            if (SDL_getenv("DOJO_HASH_EVERY")) {
                fprintf(stderr, "netplay: settled field %lu hash %016llx\n", (unsigned long)h,
                        (unsigned long long)N.hashes[h % RING]);
            }
        }
        if (N.lastHashSent && N.hashedField[N.lastHashSent % RING] == N.lastHashSent) {
            p.hashField = N.lastHashSent;
            p.hash = N.hashes[N.lastHashSent % RING];
        }
    }
    SendRaw(&p, (int)offsetof(InputPacket, inputs) + (int)(count * sizeof(uint16_t)));
}

int NetplayRunField(unsigned localButtons)
{
    int advanced = 0;

    FlushLagged();
    /* This machine's input, `delay` fields ahead of the field it is for. */
    N.localInputs[(N.field + (uint32_t)N.delay) % RING] = (uint16_t)localButtons;
    Receive();

    if (N.rollbackFrom != UINT32_MAX) {
        uint32_t f;
        SaveStateLoad(N.states[N.rollbackFrom % RING]);
        for (f = N.rollbackFrom; f < N.field; f++) {
            RunGameField(f, 1);
        }
        N.rollbacks++;
        N.rolledFields += N.field - N.rollbackFrom;
        N.rollbackFrom = UINT32_MAX;
    }

    /* Run the field unless that would take this machine too far past the other's input, or it is ahead
     * of the other: then wait a field now and then, so the two run in step. */
    {
        int32_t ahead = (int32_t)(N.field - (N.remoteNewest + N.rttMs / 2 / 17));
        int tooFar = N.field >= N.remoteKnown + MAX_ROLLBACK;
        if (tooFar || (ahead > 1 && N.field % 3 == 0)) {
            N.stalls++;
        } else {
            RunGameField(N.field, 0);
            N.field++;
            advanced = 1;
        }
    }
    Send();
    snprintf(N.status, sizeof(N.status), "player %d | delay %d | rtt %u ms | rollbacks %u (%u fields) | waits %u%s%s",
             N.local + 1, N.delay, N.rttMs, N.rollbacks, N.rolledFields, N.stalls, N.desync[0] ? " | " : "",
             N.desync);
    return advanced;
}

const char *NetplayStatus(void)
{
    return N.active ? N.status : NULL;
}

int NetplayIsSessionPacket(const void *data, int size)
{
    const InputPacket *p = (const InputPacket *)data;
    return size >= (int)offsetof(InputPacket, inputs) && p->magic == MAGIC && p->type == PACKET_INPUT;
}

static int sCardsOff;

int NetplayCardsOff(void)
{
    return N.active || sCardsOff;
}

void NetplaySetCardsOff(int off)
{
    sCardsOff = off;
}

uint32_t NetplaySettledFields(void)
{
    if (!N.active) {
        return 0;
    }
    return N.remoteKnown < N.field ? N.remoteKnown : N.field;
}

int NetplayInputsAt(uint32_t field, uint16_t *port0, uint16_t *port1)
{
    uint16_t local, remote;
    if (!N.active || field >= NetplaySettledFields() || field + RING <= N.field + (uint32_t)N.delay) {
        return 0;
    }
    local = N.localInputs[field % RING];
    remote = N.remoteInputs[field % RING];
    *port0 = N.local == 0 ? local : remote;
    *port1 = N.local == 0 ? remote : local;
    return 1;
}
