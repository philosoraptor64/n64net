/* n64net matchmaking client ("look for a match"): presence, seeking a partner, listing who is online and which
 * rooms are open. Game-neutral; talks to server/rendezvous.py (protocol in its header, "Matchmaking").
 *
 * Flow for an app (i64):
 *   - while open: n64net_match_presence() every ~30 s (state lobby / playing / away);
 *   - "Look for a match": n64net_match_seek() every ~3 s until a MATCH arrives, n64net_match_unseek() to cancel;
 *     on MATCH, the host opens room `code` and the guest joins it with the usual n64net_rendezvous_connect()
 *     (config.room = code, config.is_host from the role, max_members up to 4, force_relay = match.relay);
 *   - the lobby lists: n64net_match_list() (page 0, then more if `pages` > 1);
 *   - a host that wants others to drop in: n64net_match_open(code, 1).
 * Everything is fire-and-forget UDP on a socket the app owns: answers arrive on that socket and go through
 * n64net_match_parse(). Send on its own socket, or on the session socket and hand the server's datagrams here.
 * Every message is signed with the server password (as HOST/JOIN); invite passes don't reach matchmaking. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct N64NetMatchConfig {
    const char *server;    /* "host:port" */
    const char *password;  /* server password, or NULL/"" for an open server */
    const char *device;    /* this device's id: 6-32 letters/digits, random once and kept (n64net_match_new_device) */
    const char *name;      /* display name, UTF-8 (encoded on the wire) */
} N64NetMatchConfig;

/* A fresh random device id (16 hex digits + NUL). Generate once and store it with the app's settings. */
void n64net_match_new_device(char out[17]);

/* state: "lobby" | "seeking" | "playing" | "away"; info: NULL/"" or a short word (e.g. a game id, no spaces or
 * commas). Returns 0 if sent. */
int n64net_match_presence(int sock, const N64NetMatchConfig *config, const char *state, const char *info);
/* session: the app's room game id (both sides must use the same, e.g. "i64-session-1"); games: comma list of
 * "<game>+<build>" this device would play (a pairing needs one in common); lan: "ip:port" of `sock` on the LAN
 * (or "-"). */
int n64net_match_seek(int sock, const N64NetMatchConfig *config, const char *session, const char *games,
                      const char *lan);
int n64net_match_unseek(int sock, const N64NetMatchConfig *config);
int n64net_match_list(int sock, const N64NetMatchConfig *config, int page);
/* OPEN must go out on the room host's OWN socket (the session socket that hosted the room, also while the room
 * still waits): the server only accepts it from the room host's address (else "ERR nosuchroom"), and the OPENOK
 * comes back on that socket. Repeat until OPENOK: right after hosting, the room may not be on the server yet. */
int n64net_match_open(int sock, const N64NetMatchConfig *config, const char *room_code, int open);
/* Invitations: invite one person from the list (their `device` from PEOPLE). Repeat n64net_match_call() ~every 2 s
 * while waiting (CALLING tells how it stands); stop repeating or n64net_match_uncall() to cancel. The invited
 * person's app gets CALLED (on the socket it sends presence from) and answers with n64net_match_answer(); a yes is
 * repeated until MATCH. Nothing is made on the server until the yes: then both get MATCH (the caller hosts), as
 * with "look for a match". game: what the caller has in mind ("<game>+<build>") or NULL/"-". */
int n64net_match_call(int sock, const N64NetMatchConfig *config, const char *target_device, const char *session,
                      const char *game);
int n64net_match_answer(int sock, const N64NetMatchConfig *config, const char *caller_device, int yes);
int n64net_match_uncall(int sock, const N64NetMatchConfig *config);

enum {
    N64NET_MATCH_NONE = 0,   /* not a matchmaking answer */
    N64NET_MATCH_PRESOK,     /* online */
    N64NET_MATCH_SEEKING,    /* secs_waited, others_seeking (-1: not looking any more, after unseek) */
    N64NET_MATCH_FOUND,      /* code, is_host, game, peer_device, peer_name, relay */
    N64NET_MATCH_PEOPLE,     /* page, pages, count, people[] */
    N64NET_MATCH_ROOMS,      /* page, pages, count, rooms[] */
    N64NET_MATCH_OPENOK,     /* code, open */
    N64NET_MATCH_ERROR,      /* error: "auth", "matchversion", "bad", "busy", "nosuchroom", ... */
    N64NET_MATCH_CALLED,     /* someone invites this device: peer_device, peer_name, game ("-" = any) */
    N64NET_MATCH_CALLING,    /* the caller's invitation: peer_name, call_state ringing | declined | noanswer |
                                offline | busy | over */
    N64NET_MATCH_CALLDONE    /* the invitation from peer_device is over (cancelled, expired, answered) */
};

#define N64NET_MATCH_LIST_MAX 12

typedef struct N64NetMatchPerson {
    char name[100];     /* UTF-8 */
    char state[12];
    char info[68];
    char device[36];    /* for n64net_match_call ("" from an older server) */
} N64NetMatchPerson;

typedef struct N64NetMatchRoom {
    char code[8];
    char host_name[100];   /* UTF-8, "-" if unknown */
    char info[68];
    int members, max_members;
} N64NetMatchRoom;

typedef struct N64NetMatchEvent {
    int kind;
    int online;
    int secs_waited, others_seeking;
    char code[8];
    int is_host;
    char game[84];
    char peer_device[36];
    char peer_name[100];   /* UTF-8 */
    int relay;
    int page, pages, count;
    N64NetMatchPerson people[N64NET_MATCH_LIST_MAX];
    N64NetMatchRoom rooms[N64NET_MATCH_LIST_MAX];
    int open;
    char error[32];
    char call_state[16];
} N64NetMatchEvent;

/* Returns the kind (N64NET_MATCH_NONE if `data` isn't a matchmaking answer from the server). */
int n64net_match_parse(const void *data, int size, N64NetMatchEvent *out);

#ifdef __cplusplus
}
#endif
