/* Online play through the matchmaking server (port/server): quick play, private rooms, and King of the Hill,
 * where the winner stays on and challengers wait in a queue.
 *
 * Every match starts from the same state on both machines: the VS mode's character select, reached from
 * power-on by the same inputs, which each machine prepares itself (and both compare by hash). The offline
 * game is put aside for the match and comes back after it; while a player waits for an opponent it goes on. */

#ifndef PORT_ONLINE_H
#define PORT_ONLINE_H

typedef enum OnlineState {
    ONLINE_OFF,         /* not connected to the server */
    ONLINE_CONNECTING,
    ONLINE_IDLE,        /* connected; nothing asked for */
    ONLINE_WAITING,     /* in the quick-play queue, a private room, or a King of the Hill queue */
    ONLINE_SETUP,       /* matched: reaching the other machine */
    ONLINE_PLAYING,
} OnlineState;

typedef enum OnlineMode { ONLINE_QUICK, ONLINE_PRIVATE, ONLINE_KOTH } OnlineMode;

/* Connects to the server (settings.h's server) if not connected. */
void OnlineConnect(void);
void OnlineQuickPlay(void);
void OnlineCreateRoom(void);
void OnlineJoinRoom(const char *code);
/* King of the Hill: `code` is a private hill's, or empty for the public one. */
void OnlineKingOfTheHill(const char *code);
/* A private match player to player, without the server: hosting on settings.h's p2p_port, or joining an
 * address ("host", "host:port"). */
void OnlineHostDirect(void);
void OnlineJoinDirect(const char *address);
/* King of the Hill: those waiting in line watch the match being fought (settings.h's watch). Called each
 * host loop while watching in place of running the offline game: runs a field of the fight if one is due,
 * returning 1, or 0 (nothing to run yet: show the last picture and wait). */
int OnlineWatchField(void);
int OnlineWatching(void);
/* A match is on to watch, and watching is on: the host runs OnlineWatchField rather than the offline game. */
int OnlineWantsToWatch(void);
/* Stops watching, and stops watching until it is turned on again in the menu. */
void OnlineStopWatching(void);
/* Leaves the queue, room or match (a match is forfeited). */
void OnlineLeave(void);
/* Disconnects from the server. */
void OnlineDisconnect(void);

/* Called once a host loop: talks to the server and the other machine, prepares and starts matches, ends
 * them. Returns 1 while it is using the game itself (preparing a match's start), when the host should not
 * run it. */
int OnlineUpdate(void);
OnlineState OnlineGetState(void);
OnlineMode OnlineGetMode(void);
int OnlineInMatch(void);
/* Lines describing where online play is, for the menu; and a short one for during a match or a wait. */
void OnlineDescribe(char *out, int size);
void OnlineBanner(char *out, int size);
/* Replays: every online match is saved to replays/ when it ends. Lists the newest first (file names, up to
 * `max`); plays one, as a spectator watches, from the starting state this game makes (only a replay this
 * version made plays right). Returns 0 with a message if it can't. */
int OnlineListReplays(char names[][96], int max);
int OnlinePlayReplay(const char *file);
/* A message that should be shown once (an error, a result), or NULL. */
const char *OnlineTakeMessage(void);

#endif
