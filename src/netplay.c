/* Online play with rollback (netplay.h).
 *
 * The unit is the field, the emulated vertical blank the game yields at (kernel.c): each field the two
 * controllers are written and the game runs to the next one. The simulation is a function of those inputs
 * alone (DOJO_HASH_EVERY shows it: the same inputs give the same state on any machine), so each machine
 * runs both players: its own with its input `delay` fields late, the other's predicted as the last input it
 * has, and when the real input arrives and differs, it loads the state saved before that field and runs
 * forward again with drawing and sound left out.
 *
 * A field's input is a sample of the controller for each of its ticks (NetplaySamplesPerField: one at 60 fps,
 * four at 240), written to the game before that tick, so a press counts from the next tick, not the next
 * field. This machine samples its own before each tick it runs, for the same tick `delay` fields on.
 *
 * Packets are UDP and carry every input the other machine has not acknowledged, so a lost packet costs
 * nothing (each field as its first sample and the ticks it changes at); each also carries the sender's newest
 * field, for keeping the two in step, and from time to time the hash of a field both have settled, for
 * noticing a desync. */

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
#include "pc_substep.h"
#include "netplay.h"
#include "savestate.h"
#include "spu.h"

#define MAGIC          0x504E4242u /* "BBNP" */
#define VERSION        5
#define RING           64          /* fields of inputs and states kept */
#define MAX_ROLLBACK   12          /* how far a machine may run on predictions */
#define MAX_SEND       60          /* fields a packet carries at most */
#define MAX_SEND_BYTES 400         /* and their encoding's bytes (online.c queues packets of 512) */
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
    uint8_t inputs[MAX_SEND_BYTES]; /* `count` fields: the first sample, the changes, (tick, sample) each */
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
    int samples;             /* controller samples a field (NetplaySamplesPerField) */
    uint32_t field;          /* the next field to run */
    uint16_t localInputs[RING][NETPLAY_MAX_SAMPLES];
    uint16_t remoteInputs[RING][NETPLAY_MAX_SAMPLES];
    uint16_t usedRemote[RING][NETPLAY_MAX_SAMPLES];
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
int NetplaySamplesPerField(int ticksPerFrame)
{
    int samples = (ticksPerFrame + 1) / 2; /* a console frame is two fields */
    return samples < 1 ? 1 : (samples > NETPLAY_MAX_SAMPLES ? NETPLAY_MAX_SAMPLES : samples);
}

static void BeginSession(int ticksPerFrame)
{
    int i;
    N.samples = NetplaySamplesPerField(ticksPerFrame);
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
    BeginSession(g_pcTicksPerFrame);
    fprintf(stderr, "netplay: session as player %d, input delay %d fields, %d samples a field\n", N.local + 1,
            N.delay, N.samples);
}

void NetplayEnd(void)
{
    N.active = 0;
    SpuEndFields();
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
    BeginSession(*agreedTicks);
    fprintf(stderr, "netplay: connected as player %d, input delay %d fields, %d samples a field\n", N.local + 1,
            N.delay, N.samples);
    return 1;
}

int NetplayActive(void)
{
    return N.active;
}

/* The other machine's samples for a field: known, or predicted, all as the newest sample known before it. */
static void RemoteInputFor(uint32_t field, uint16_t *out)
{
    int i;
    uint16_t last;
    if (field < N.remoteKnown) {
        memcpy(out, N.remoteInputs[field % RING], sizeof(N.remoteInputs[0]));
        return;
    }
    last = N.remoteKnown > 0 ? N.remoteInputs[(N.remoteKnown - 1) % RING][N.samples - 1] : 0;
    for (i = 0; i < NETPLAY_MAX_SAMPLES; i++) {
        out[i] = last;
    }
}

unsigned NetplayFilterButtons(unsigned buttons)
{
    extern short gGameState;
    return gGameState == 2 /* GAME_STATE_UPDATE_COMBAT */ ? buttons & ~(1u << 11) : buttons;
}

/* Runs one field: saves the state before it, and lets the game run to the next vertical blank, through any
 * ticks that end before it, each with both controllers written from its samples (each shown, unless the field
 * is run again after a rollback, which is neither drawn nor heard). Run for the first time, it samples this
 * machine's controller before each tick after the first, for the field `delay` on. */
static void RunGameField(uint32_t field, int again)
{
    uint8_t *state = N.states[field % RING];
    const uint16_t *local = N.localInputs[field % RING];
    uint16_t *remote = N.usedRemote[field % RING];
    const int16_t *samples;
    int frames, tick;

    SaveStateSave(state);
    N.stateField[field % RING] = field;
    RemoteInputFor(field, remote);
    SpuBeginField((long)field, !again);
    for (tick = 0;; tick++) {
        int sample = tick < N.samples ? tick : N.samples - 1;
        if (!again && tick > 0 && tick < N.samples) {
            N.localInputs[(field + (uint32_t)N.delay) % RING][tick] = (uint16_t)HostLocalButtons();
        }
        HostWritePadPort(N.local, 1, NetplayFilterButtons(local[sample]), NULL);
        HostWritePadPort(1 - N.local, 1, NetplayFilterButtons(remote[sample]), NULL);
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
        {
            const uint8_t *in = p.inputs, *end = (const uint8_t *)&p + got;
            for (i = 0; i < p.count && i < MAX_SEND; i++) {
                uint32_t field = p.first + i;
                uint16_t decoded[NETPLAY_MAX_SAMPLES];
                int changes, c, t;
                if (in + 3 > end) {
                    break;
                }
                decoded[0] = (uint16_t)(in[0] | in[1] << 8);
                changes = in[2];
                in += 3;
                if (in + changes * 3 > end) {
                    break;
                }
                for (t = 1; t < NETPLAY_MAX_SAMPLES; t++) {
                    decoded[t] = decoded[0];
                }
                for (c = 0; c < changes; c++, in += 3) {
                    for (t = in[0]; t > 0 && t < NETPLAY_MAX_SAMPLES; t++) {
                        decoded[t] = (uint16_t)(in[1] | in[2] << 8);
                    }
                }
                if (field != N.remoteKnown) {
                    continue; /* had it already, or a gap: inputs are taken in order */
                }
                memcpy(N.remoteInputs[field % RING], decoded, sizeof(decoded));
                N.remoteKnown = field + 1;
                /* A field already run on a prediction that was wrong: run again from there. */
                if (field < N.field && N.stateField[field % RING] == field &&
                    memcmp(N.usedRemote[field % RING], decoded, (size_t)N.samples * sizeof(uint16_t)) != 0 &&
                    field < N.rollbackFrom) {
                    N.rollbackFrom = field;
                }
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
            if (SDL_getenv("DOJO_DUMP_DESYNC") && N.stateField[p.hashField % RING] == p.hashField) {
                /* This machine's state before that field, for tools/statediff.py against the other's. */
                char name[64];
                FILE *f;
                snprintf(name, sizeof(name), "desync_p%d_%06lu.bin", N.local + 1, (unsigned long)p.hashField);
                if ((f = fopen(name, "wb")) != NULL) {
                    fwrite(N.states[p.hashField % RING], 1, SaveStateCapacity(), f);
                    fclose(f);
                    SaveStatePrintRegions(stderr);
                }
            }
        }
    }
}

static void Send(void)
{
    InputPacket p;
    uint32_t newestLocal = N.field + (uint32_t)N.delay; /* local inputs exist up to here */
    uint32_t first = N.remoteAcked, count;
    int used = 0;
    memset(&p, 0, sizeof(p));
    if (newestLocal - first > MAX_SEND) {
        first = newestLocal - MAX_SEND;
    }
    count = newestLocal - first;
    p.magic = MAGIC;
    p.type = PACKET_INPUT;
    p.first = first;
    p.ack = N.remoteKnown;
    p.newest = N.field;
    p.sentMs = NowMs();
    p.echoMs = N.peerSentMs ? N.peerSentMs + (NowMs() - N.peerSentAt) : 0;
    {
        /* Each field: its first sample, then each tick its sample changes at, as far as the bytes allow. */
        uint32_t i;
        for (i = 0; i < count; i++) {
            const uint16_t *in = N.localInputs[(first + i) % RING];
            uint8_t *out = p.inputs + used;
            int t, changes = 0;
            for (t = 1; t < N.samples; t++) {
                changes += in[t] != in[t - 1];
            }
            if (used + 3 + changes * 3 > MAX_SEND_BYTES) {
                break;
            }
            out[0] = (uint8_t)in[0];
            out[1] = (uint8_t)(in[0] >> 8);
            out[2] = (uint8_t)changes;
            out += 3;
            for (t = 1; t < N.samples; t++) {
                if (in[t] != in[t - 1]) {
                    out[0] = (uint8_t)t;
                    out[1] = (uint8_t)in[t];
                    out[2] = (uint8_t)(in[t] >> 8);
                    out += 3;
                }
            }
            used += 3 + changes * 3;
        }
        p.count = (uint16_t)i;
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
            if (SDL_getenv("DOJO_DUMP_SETTLED") && (strtoul(SDL_getenv("DOJO_DUMP_SETTLED"), NULL, 10) == h ||
                                                   (SDL_getenv("DOJO_DUMP_SETTLED")[0] == 'a' && h <= 600))) {
                /* the state before that settled field, for tools/statediff.py (a spectator's: DOJO_DUMP_WATCH) */
                char name[64];
                FILE *f;
                snprintf(name, sizeof(name), "settled_p%d_%06lu.bin", N.local + 1, (unsigned long)h);
                if ((f = fopen(name, "wb")) != NULL) {
                    fwrite(N.states[h % RING], 1, SaveStateCapacity(), f);
                    fclose(f);
                    SaveStatePrintRegions(stderr);
                }
            }
        }
        if (N.lastHashSent && N.hashedField[N.lastHashSent % RING] == N.lastHashSent) {
            p.hashField = N.lastHashSent;
            p.hash = N.hashes[N.lastHashSent % RING];
        }
    }
    SendRaw(&p, (int)offsetof(InputPacket, inputs) + used);
}

int NetplayRunField(unsigned localButtons)
{
    int advanced = 0;

    FlushLagged();
    /* This machine's input, `delay` fields ahead of the field it is for: its first tick's sample, and the
     * rest's until the ticks before them take their own (RunGameField). */
    {
        int t;
        for (t = 0; t < NETPLAY_MAX_SAMPLES; t++) {
            N.localInputs[(N.field + (uint32_t)N.delay) % RING][t] = (uint16_t)localButtons;
        }
    }
    Receive();

    if (N.rollbackFrom != UINT32_MAX) {
        uint32_t f;
        SaveStateLoad(N.states[N.rollbackFrom % RING]);
        for (f = N.rollbackFrom; f < N.field; f++) {
            RunGameField(f, 1);
        }
        SpuRestoreAfterRollback((long)N.rollbackFrom, (long)N.field);
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
    snprintf(N.status, sizeof(N.status),
             "player %d | delay %d | rtt %u ms | rollbacks %u (%u fields) | sounds restored %u | waits %u%s%s",
             N.local + 1, N.delay, N.rttMs, N.rollbacks, N.rolledFields, SpuRestoredSounds(), N.stalls,
             N.desync[0] ? " | " : "", N.desync);
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

int NetplayOnlineSimulation(void)
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
    const uint16_t *local, *remote;
    if (!N.active || field >= NetplaySettledFields() || field + RING <= N.field + (uint32_t)N.delay) {
        return 0;
    }
    local = N.localInputs[field % RING];
    remote = N.remoteInputs[field % RING];
    memcpy(port0, N.local == 0 ? local : remote, (size_t)N.samples * sizeof(uint16_t));
    memcpy(port1, N.local == 0 ? remote : local, (size_t)N.samples * sizeof(uint16_t));
    return N.samples;
}
