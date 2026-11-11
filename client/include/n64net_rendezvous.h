/* n64net rendezvous client: find the other player through a room code.
 *
 * Game-neutral C API. Runs on the game's own UDP socket, so the NAT mapping it opens
 * is the one gameplay packets use afterwards. See
 * server/rendezvous.py for the protocol.
 */
#pragma once

#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct N64NetRendezvousConfig {
    const char *server;      /* "host:port"; host may be a DNS name */
    const char *game;        /* "<game>+<build>": only equal ids pair up; a
                                differing build fails with error "version" */
    const char *room;        /* 4-char code; NULL/"" lets the host get a random one */
    const char *password;    /* server password, or NULL/"" for an open server */
    int is_host;             /* 1 = create the room, 0 = join it */
    int force_relay;         /* skip hole punching (testing) */
    int wait_secs;           /* how long to wait for the other player (default 600) */
    void (*log)(const char *fmt, ...);
    void (*yield)(void);     /* called every ~10ms while waiting (watchdog etc.) */
    /* Guest without the password: an invite pass for `room` (from the host's
     * n64net_rendezvous_request_invite). Used only when `password` is empty. */
    const char *invite_pass;
    /* Optional: set *cancel to non-zero (from any thread) to stop waiting for
     * the other player; connect then fails with error "cancelled". */
    const volatile int *cancel;
} N64NetRendezvousConfig;

typedef struct N64NetRendezvousResult {
    struct sockaddr_in peer; /* where to send gameplay packets */
    int relayed;             /* 1 = peer is the server, which forwards */
    /* The room server. Accept the peer's packets from EITHER `peer` or
     * `server` (non-control datagrams from the server are the peer's,
     * relayed): in rare cases one side ends up direct and the other relayed,
     * which then still works both ways. */
    struct sockaddr_in server;
    int is_host;             /* role (a joiner is never host) */
    char room[8];
    /* On failure, why (for the player): "version <host_build>", "auth",
     * "noroom" (the code was never hosted), "timeout", "cancelled", "full", "taken",
     * "game", "badcode", "busy", "resolve", "relay", "args". */
    char error[64];
} N64NetRendezvousResult;

/* Blocks until connected, failed or timed out. Returns 0 on success.
 * `sock` must be a bound, non-blocking IPv4 UDP socket. */
int n64net_rendezvous_connect(int sock, const N64NetRendezvousConfig *config, N64NetRendezvousResult *out);

/* Host leaving a room (e.g. after a match): tells the server to close it, so
 * a joiner waiting for the next match pairs with the host's next room rather
 * than this one. Fire-and-forget (sent a few times). */
void n64net_rendezvous_bye(int sock, const N64NetRendezvousConfig *config);

/* Invite passes (so invite links needn't carry the password). The host
 * (password set in `config`, room = its open room) sends a request; the
 * server answers on the same socket with a control datagram that whoever
 * reads the socket hands to n64net_rendezvous_parse_invite(). Returns 0 if
 * sent. A pass lets a guest JOIN that room only, until it expires
 * (ttl_secs, default 24 h, max 7 days). */
int n64net_rendezvous_request_invite(int sock, const N64NetRendezvousConfig *config, int ttl_secs);
/* Returns 1 if `data` is the server's invite answer: fills room_out (the
 * code) and pass_out (put it in the guest's config.invite_pass). Else 0. */
int n64net_rendezvous_parse_invite(const void *data, int size, char room_out[8], char *pass_out, int pass_cap);

#ifdef __cplusplus
}
#endif
