/* n64net_input test: two peers in one process play a small deterministic
 * game over a simulated network (latency, jitter, loss) with a virtual clock.
 * Checks that both end in the same state at every confirmed tick (sync
 * checks), in lockstep and in rollback mode, with a run-time delay change.
 *
 *   cmake -S . -B build -DN64NET_BUILD_TESTS=ON && cmake --build build && build/n64net_input_test */
#include "n64net_input.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

namespace {

uint64_t gNow = 0;  // virtual clock, usecs
uint64_t Now() { return gNow; }

uint32_t gRand = 12345;
uint32_t Rand() {
    gRand = gRand * 1103515245u + 12345u;
    return gRand >> 8;
}

struct Packet {
    uint64_t at;
    int to;
    std::vector<uint8_t> data;
};
std::deque<Packet> gWire;
int gLossPercent = 0, gLatencyUsecs = 0, gJitterUsecs = 0;
uint32_t gLateStart = 0;      // peer 1 starts this many frames late
uint32_t gCorruptTick = 0;
uint32_t gAutoMax = 0;        // auto delay [1, gAutoMax] when non-zero    // peer 1's state is nudged at this tick (desync test)

struct Game {  // the "simulation": two points moved by the pads, plus an RNG
    int32_t x[2], y[2];
    uint32_t rng;
    uint32_t tick;
    void Step(const n64net_pad pads[2]) {
        for (int p = 0; p < 2; p++) {
            x[p] += pads[p].stick_x + 3 * pads[p].rstick_x;
            y[p] += pads[p].stick_y - 5 * pads[p].rstick_y;
            if (pads[p].buttons & 1) { rng = rng * 69069u + 1u; x[p] ^= (int32_t)(rng >> 20); }
            if (pads[p].buttons_hi & 1) y[p] ^= 0x5a5a;
        }
        tick++;
    }
    uint32_t Hash() const {
        uint32_t h = 2166136261u;
        const uint8_t *b = (const uint8_t *)this;
        for (size_t i = 0; i < sizeof(*this); i++) h = (h ^ b[i]) * 16777619u;
        return h;
    }
};

struct Peer {
    int id;
    n64net_input *net;
    Game game{};
    std::vector<Game> slots;
    uint32_t pad_seed;
    int frames_waited = 0;
    uint32_t hashes[256] = {};  // state hash after reaching tick t (resims rewrite)
    uint32_t next_check = 10;
};
Peer gPeers[2];

void SendFrom0(void *, const uint8_t *d, uint32_t n);
void SendFrom1(void *, const uint8_t *d, uint32_t n);
void Queue(int to, const uint8_t *d, uint32_t n) {
    if ((int)(Rand() % 100) < gLossPercent) return;
    uint64_t lat = gLatencyUsecs + (gJitterUsecs ? Rand() % gJitterUsecs : 0);
    gWire.push_back(Packet{gNow + lat, to, std::vector<uint8_t>(d, d + n)});
}
void SendFrom0(void *, const uint8_t *d, uint32_t n) { Queue(1, d, n); }
void SendFrom1(void *, const uint8_t *d, uint32_t n) { Queue(0, d, n); }

void Deliver() {
    for (auto it = gWire.begin(); it != gWire.end();) {
        if (it->at <= gNow) {
            n64net_input_handle_packet(gPeers[it->to].net, it->data.data(), (uint32_t)it->data.size());
            it = gWire.erase(it);
        } else {
            ++it;
        }
    }
}

void Save(void *u, uint32_t slot) {
    Peer *p = (Peer *)u;
    if (n64net_rollback_callback_tick(p->net) != p->game.tick) printf("FAIL callback tick %u vs game %u\n", n64net_rollback_callback_tick(p->net), p->game.tick);
    p->slots[slot] = p->game;
}
void Load(void *u, uint32_t slot) { Peer *p = (Peer *)u; p->game = p->slots[slot]; }
void Advance(void *u) {
    Peer *p = (Peer *)u;
    n64net_pad pads[2];
    for (int i = 0; i < 2; i++) n64net_input_get(p->net, i, p->game.tick, &pads[i]);
    p->game.Step(pads);
    p->hashes[p->game.tick % 256] = p->game.Hash();
}

n64net_pad PadFor(Peer &p, uint32_t tick) {
    // Each player changes input every few ticks (like a human), deterministic per peer.
    uint32_t s = p.pad_seed ^ (tick / 7) * 2654435761u;
    s ^= s >> 13; s *= 0x5bd1e995u; s ^= s >> 15;
    n64net_pad pad = {};
    pad.buttons = (uint16_t)(s & 0x0003);
    pad.stick_x = (int8_t)((int)((s >> 8) % 161) - 80);
    pad.stick_y = (int8_t)((int)((s >> 16) % 161) - 80);
    // Right stick: player 0 uses it in bursts (packets switch between the old 4-byte frames and the
    // right-stick format), player 1 never does (old format only).
    if (p.id == 0 && ((s >> 24) & 3) != 0) {
        pad.rstick_x = (int8_t)((int)((s >> 4) % 161) - 80);
        pad.rstick_y = (int8_t)((int)((s >> 20) % 161) - 80);
        pad.buttons_hi = (uint16_t)((s >> 27) & 1);
    }
    return pad;
}

void Log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
}

// Runs `frames` display frames (16.7 ms) on both peers; returns 0 if OK.
int Run(const char *name, uint32_t window, uint32_t delay, int loss, int latency_ms, int jitter_ms, uint32_t frames,
        bool change_delay) {
    gWire.clear();
    gNow = 1000000;
    gLossPercent = loss;
    gLatencyUsecs = latency_ms * 1000;
    gJitterUsecs = jitter_ms * 1000;
    const uint32_t kSlots = 64;
    for (int i = 0; i < 2; i++) {
        Peer &p = gPeers[i];
        p = Peer{};
        p.id = i;
        p.pad_seed = 0x1000u + (uint32_t)i * 77u;
        p.slots.assign(kSlots, Game{});
        n64net_input_config c{};
        c.num_players = 2;
        c.local_player = i;
        c.session_id = 42;
        c.input_delay = delay;
        c.rollback_window = window;
        c.ticks_per_second = 60;
        c.send = i == 0 ? SendFrom0 : SendFrom1;
        c.user = &p;
        c.log = Log;
        c.now_usecs = Now;
        p.net = n64net_input_create(&c);
        if (gAutoMax) n64net_input_set_auto_delay(p.net, 1, gAutoMax);
    }
    uint32_t stalls[2] = {0, 0};
    for (uint32_t f = 0; f < frames; f++) {
        gNow += 16667;
        Deliver();
        for (int i = 0; i < 2; i++) {
            Peer &p = gPeers[i];
            if (i == 1 && f < gLateStart) continue;
            if (change_delay && p.game.tick == 600) n64net_input_set_delay(p.net, delay + 3);
            if (change_delay && p.game.tick == 1200) n64net_input_set_delay(p.net, delay);
            if (window && n64net_timesync_should_skip(p.net)) continue;
            const uint32_t tick = p.game.tick;
            if (window) {
                n64net_rollback_callbacks cb{Save, Load, Advance, &p, kSlots};
                if (n64net_rollback_frame(p.net, tick, &cb) < 0) {
                    printf("%s: FAIL rollback too deep on peer %d\n", name, i);
                    return 1;
                }
            }
            // Sample once per tick (a waiting frame doesn't sample again).
            if (p.frames_waited == 0) n64net_input_set_local(p.net, tick, PadFor(p, tick));
            if (!n64net_input_ready(p.net, tick)) {
                n64net_input_resend(p.net);
                p.frames_waited++;
                stalls[i]++;
                continue;
            }
            p.frames_waited = 0;
            Advance(&p);
            if (i == 1 && gCorruptTick && p.game.tick == gCorruptTick) {
                p.game.x[0]++;
                p.hashes[p.game.tick % 256] = p.game.Hash();
            }
            // Sync check: once the inputs of every tick before T are confirmed,
            // the state at T can no longer change: send its hash.
            const uint32_t c = n64net_input_confirmed_tick(p.net);
            while (c != UINT32_MAX && p.next_check <= c + 1 && p.next_check <= p.game.tick) {
                n64net_sync_record(p.net, p.next_check, 0, p.hashes[p.next_check % 256]);
                p.next_check += 10;
            }
        }
    }
    // Drain: let both catch up and confirm, then compare final states at equal ticks.
    for (int f = 0; f < 600; f++) {
        gNow += 16667;
        Deliver();
        for (int i = 0; i < 2; i++) n64net_input_resend(gPeers[i].net);
    }
    Deliver();
    int rc = 0;
    uint32_t dtick;
    uint8_t dkind;
    n64net_input_stats st[2];
    if (gCorruptTick) {
        // Expected: a desync reported on both sides.
        const int seen = n64net_sync_desynced(gPeers[0].net, &dtick, &dkind) + n64net_sync_desynced(gPeers[1].net, &dtick, &dkind);
        rc = seen == 2 ? 0 : 1;
        printf("%s: %s (desync seen by %d peers at tick %u)\n", name, rc ? "FAIL" : "ok", seen, dtick);
        for (int i = 0; i < 2; i++) n64net_input_destroy(gPeers[i].net);
        return rc;
    }
    for (int i = 0; i < 2; i++) {
        n64net_input_get_stats(gPeers[i].net, &st[i]);
        if (n64net_sync_desynced(gPeers[i].net, &dtick, &dkind)) {
            printf("%s: FAIL desync on peer %d at tick %u\n", name, i, dtick);
            rc = 1;
        }
    }
    const uint32_t matched = n64net_sync_matched(gPeers[0].net) + n64net_sync_matched(gPeers[1].net);
    if (matched < 20) {
        printf("%s: FAIL only %u sync checks matched\n", name, matched);
        rc = 1;
    }
    if (gAutoMax) printf("%s: final delays %u/%u\n", name, n64net_input_get_delay(gPeers[0].net), n64net_input_get_delay(gPeers[1].net));
    printf("%s: %s ticks %u/%u, sync matched %u, stalls %u/%u, predictions %u, mispredictions %u, rollbacks %u/%u, "
           "skips %u/%u, rtt avg %u us\n",
           name, rc ? "FAIL" : "ok", gPeers[0].game.tick, gPeers[1].game.tick, matched, stalls[0], stalls[1],
           st[0].predictions + st[1].predictions, st[0].mispredictions + st[1].mispredictions, st[0].rollbacks,
           st[1].rollbacks, st[0].timesync_skips, st[1].timesync_skips, st[0].rtt_avg_usecs);
    for (int i = 0; i < 2; i++) n64net_input_destroy(gPeers[i].net);
    return rc;
}

} // namespace

int main() {
    int rc = 0;
    rc |= Run("lockstep clean", 0, 2, 0, 5, 0, 3000, false);
    rc |= Run("lockstep lossy", 0, 3, 10, 30, 20, 3000, false);
    rc |= Run("rollback clean", 8, 1, 0, 5, 0, 3000, false);
    rc |= Run("rollback 50ms+30 jitter 10% loss", 8, 2, 10, 50, 30, 3000, false);
    rc |= Run("rollback delay change", 8, 2, 5, 40, 20, 3000, true);
    rc |= Run("lockstep delay change", 0, 2, 5, 20, 10, 3000, true);
    rc |= Run("lockstep 50ms+20 jitter, fixed delay 1", 0, 1, 2, 50, 20, 3000, false);
    gAutoMax = 8;
    rc |= Run("lockstep 50ms+20 jitter, auto delay", 0, 1, 2, 50, 20, 3000, false);
    rc |= Run("rollback 50ms+20 jitter, auto delay", 8, 1, 2, 50, 20, 3000, false);
    gAutoMax = 0;
    // rollback with a long round trip: sender ahead of the peer's ack by more than one packet's frames
    // (window 8 + delay 3 + ~10 ticks of ack latency); used to stall in bursts (MP2 on an iPhone)
    rc |= Run("rollback 80ms+40 jitter 5% loss, delay 3", 8, 3, 5, 80, 40, 3000, false);
    rc |= Run("rollback 120ms+40 jitter 5% loss, delay 3", 8, 3, 5, 120, 40, 3000, false);
    gLateStart = 20;
    rc |= Run("rollback, peer 1 starts 20 frames late (time sync)", 8, 2, 0, 20, 5, 3000, false);
    gLateStart = 0;
    gCorruptTick = 500;
    rc |= Run("desync detection", 8, 2, 0, 20, 5, 1500, false);
    gCorruptTick = 0;
    printf(rc ? "SOME TESTS FAILED\n" : "ALL PASSED\n");
    return rc;
}
