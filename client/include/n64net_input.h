/* n64net input exchange: the input ledger, prediction, rollback trigger,
 * time sync, sync checks and away/back notices, for one online match.
 * Game-neutral C API (client/src/n64net_input.cpp).
 *
 * Model. A tick is one step of the game's simulation that reads controllers
 * once. Every machine owns one player; its controller sample taken at tick t
 * is applied by every machine at tick t + input_delay. Remote inputs that
 * haven't arrived are predicted (the last confirmed input repeats). When a
 * remote input arrives for a tick that was already simulated with a wrong
 * prediction, the earliest such tick is reported as a rollback: the game
 * restores its state from before that tick and resimulates up to the present
 * (n64net_rollback_frame helps with that). rollback_window bounds how far the
 * game may run ahead of the confirmed inputs; 0 means lockstep (never
 * predict: wait for every input).
 *
 * The library does not own a socket: it calls config.send() with datagrams
 * for the other machine(s) and the game passes every received datagram to
 * n64net_input_handle_packet() (it returns 0 for datagrams that aren't its
 * own, magic 'N64I', so they can share a socket with the game's own packets
 * and with voice). Everything is called from the game thread.
 *
 * Per tick (rollback):
 *     n64net_rollback_frame(net, tick, &callbacks);   // performs a pending rollback, saves state
 *     n64net_input_set_local(net, tick, my_pad);       // sample; sent to the peer
 *     while (!n64net_input_ready(net, tick)) { receive packets; n64net_input_resend(net); wait }
 *     for each player p: n64net_input_get(net, p, tick, &pad)  -> run the tick
 *     n64net_sync_record(net, tick, kind, hash);       // now and then
 * and once per displayed frame, before the tick: if (n64net_timesync_should_skip(net)) skip
 * the game step this frame (re-present the last frame). */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define N64NET_INPUT_MAGIC 0x4E363449u /* 'N64I' */
#define N64NET_INPUT_MAX_PLAYERS 4
#define N64NET_INPUT_MAX_PACKET 192

typedef struct n64net_pad {
    uint16_t buttons;
    int8_t stick_x;
    int8_t stick_y;
} n64net_pad;

typedef struct n64net_input n64net_input;

typedef struct n64net_input_config {
    int num_players;           /* 2..N64NET_INPUT_MAX_PLAYERS, players 0..n-1 */
    int local_player;          /* the player this machine controls */
    uint32_t remote_mask;      /* players driven by other machines (bit p); 0 = all but local_player.
                                  Players in neither (CPUs, empty slots) read as neutral input */
    uint32_t session_id;       /* equal on both machines; packets from other sessions are ignored */
    uint32_t start_tick;       /* first tick of the match (inputs before it are never needed) */
    uint32_t input_delay;      /* ticks between sampling and applying local input (>= 0) */
    uint32_t rollback_window;  /* max ticks simulated past the confirmed inputs; 0 = lockstep */
    uint32_t ticks_per_second; /* the game's tick rate (time sync pacing); default 60 */
    void (*send)(void *user, const uint8_t *data, uint32_t size);
    void *user;
    void (*log)(const char *fmt, ...);  /* optional */
    uint64_t (*now_usecs)(void);        /* optional monotonic clock (tests); default steady clock */
} n64net_input_config;

n64net_input *n64net_input_create(const n64net_input_config *config);
void n64net_input_destroy(n64net_input *net);

/* The local controller sampled at `tick` (call once per tick, before
 * n64net_input_get for that tick; never during a resimulation). It is applied
 * at tick + input_delay. The first input_delay ticks after start_tick are
 * neutral (sent from the start, so lockstep can't deadlock on them); with a
 * delay of 0, lockstep needs each side to sample before waiting. Sends an
 * input packet. */
void n64net_input_set_local(n64net_input *net, uint32_t tick, n64net_pad pad);

/* 1 if `tick` may be simulated now: every remote player's inputs are confirmed
 * up to at least tick - rollback_window (lockstep: up to tick). */
int n64net_input_ready(n64net_input *net, uint32_t tick);

/* The input of `player` for `tick`. Returns 1 if it is a prediction (a
 * remote input not received yet), else 0. The library remembers predictions
 * it handed out, to detect when a late input differs. */
int n64net_input_get(n64net_input *net, int player, uint32_t tick, n64net_pad *out);

/* Feed every received datagram. Returns 1 if it was an n64net input packet
 * (consumed), 0 otherwise. */
int n64net_input_handle_packet(n64net_input *net, const uint8_t *data, uint32_t size);

/* While waiting (n64net_input_ready returned 0): resends our latest inputs at
 * most every 10 ms, so a lost packet doesn't stall both sides. */
void n64net_input_resend(n64net_input *net);

/* If a remote input arrived for an already-simulated tick and differs from
 * the prediction used, returns 1 and the earliest such tick (once). */
int n64net_take_rollback(n64net_input *net, uint32_t *tick);

/* Like n64net_take_rollback, without consuming it. */
int n64net_peek_rollback(n64net_input *net, uint32_t *tick);

/* Switch between lockstep (0) and rollback at a tick boundary, e.g. lockstep
 * menus and rollback battles in one session. */
void n64net_input_set_window(n64net_input *net, uint32_t rollback_window);

/* Highest tick whose inputs are all confirmed (UINT32_MAX before any). */
uint32_t n64net_input_confirmed_tick(n64net_input *net);

/* Change the local input delay at run time (per side; the peer needs no
 * notice). Growing it repeats the last sample for the skipped ticks;
 * shrinking it drops samples that would land on already-scheduled ticks. */
void n64net_input_set_delay(n64net_input *net, uint32_t input_delay);

/* Automatic input delay: each side wishes for enough delay to cover the
 * one-way time of the 90th percentile of recent round trips (less one tick),
 * within [min_delay, max_delay]; both apply the larger of the two wishes, up
 * at once, down one tick after 8 calm seconds. max_delay 0 turns it off. In
 * lockstep this trades a little input lag for far fewer stalls; with
 * rollback, keep max_delay low (rollback hides the rest). */
void n64net_input_set_auto_delay(n64net_input *net, uint32_t min_delay, uint32_t max_delay);
uint32_t n64net_input_get_delay(n64net_input *net);

/* --- Time sync -------------------------------------------------------------
 * Each side measures its advantage (its tick minus the newest remote tick it
 * has) and sends it with every input packet. When this side runs ahead of the
 * peer by 1.5+ ticks on average, this returns 1 at most every half second:
 * the game then skips one game step (re-presents the last frame), letting the
 * peer catch up. Call once per displayed frame, outside resimulation. */
int n64net_timesync_should_skip(n64net_input *net);

/* --- Sync check ------------------------------------------------------------
 * Record a hash of the game state after `tick` (kind: any small number, e.g.
 * 0 = battle, 1 = menus; compared only with the same kind). Record only
 * states whose inputs are all confirmed (tick <= n64net_input_confirmed_tick)
 * so a later rollback can't change them. Hashes travel to the peer; the
 * first mismatch is reported once. */
void n64net_sync_record(n64net_input *net, uint32_t tick, uint8_t kind, uint32_t hash);
/* 1 once a mismatch was seen; tick/kind of the first one. */
int n64net_sync_desynced(n64net_input *net, uint32_t *tick, uint8_t *kind);
uint32_t n64net_sync_matched(n64net_input *net); /* checks that matched so far */

/* --- Away / back -----------------------------------------------------------
 * The app is going to the background (iOS) or coming back: tells the peer,
 * which should wait longer before giving up (n64net_input_peer_away). */
void n64net_input_set_away(n64net_input *net, int away);
int n64net_input_peer_away(n64net_input *net);
/* Microseconds since the last packet from the peer (0 before any). */
uint64_t n64net_input_peer_silence_usecs(n64net_input *net);

/* --- Stats -----------------------------------------------------------------*/
typedef struct n64net_input_stats {
    uint32_t packets_sent, packets_received, packets_dropped; /* dropped: bad/foreign */
    uint32_t predictions, mispredictions, rollbacks;
    uint32_t rtt_avg_usecs, rtt_max_usecs;   /* recent round trips */
    int32_t local_advantage, remote_advantage;
    uint32_t timesync_skips;
} n64net_input_stats;
void n64net_input_get_stats(n64net_input *net, n64net_input_stats *out);

/* --- Rollback driver (optional) -------------------------------------------
 * For games that snapshot their whole simulation. Call at the start of every
 * simulated frame with the tick about to run: if a rollback is pending, it
 * loads the state saved before the earliest mispredicted tick and calls
 * advance() once per tick up to (not including) `tick`, saving each state on
 * the way; then it saves the state before `tick`. advance() must run one game
 * tick (reading inputs with n64net_input_get, without drawing or sound) and
 * move the game's tick forward. Slots are tick % slot_count; slot_count must
 * exceed rollback_window + input_delay. Returns the rollback depth (0 = none),
 * or -1 if the needed state was older than the ring (the peers may now differ). */
typedef struct n64net_rollback_callbacks {
    void (*save)(void *user, uint32_t slot);
    void (*load)(void *user, uint32_t slot);
    void (*advance)(void *user);
    void *user;
    uint32_t slot_count;
} n64net_rollback_callbacks;
int n64net_rollback_frame(n64net_input *net, uint32_t tick, const n64net_rollback_callbacks *callbacks);
/* Inside a save/load/advance callback: the tick it is about (save/load: the
 * state before that tick; advance: the tick being run). */
uint32_t n64net_rollback_callback_tick(n64net_input *net);
/* 1 while n64net_rollback_frame is resimulating (inputs come from the ledger;
 * skip drawing, sound and n64net_input_set_local). */
int n64net_rollback_resimulating(n64net_input *net);

#ifdef __cplusplus
}
#endif
