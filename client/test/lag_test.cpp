// One laggy machine (todo item 11): 3-4 machines, one of them on a slow link to everyone. Auto delay and time
// sync on, as in the games. The others must not wait (stall) or slow down (time sync skips) because of it, and
// should roll back little; the laggy machine carries the lag. Every machine must hold the same states.
// Also: 2 machines on a slow link (both "laggy": they share it, as before), and all-normal links.
//   clang++ -std=c++17 -O1 -Iclient/include client/src/n64net_input.cpp client/test/lag_test.cpp -o lag_test
//   ./lag_test
#include "n64net_input.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

namespace {

uint64_t g_now = 0;
uint64_t Now() { return g_now; }
uint32_t g_rng = 4242;
uint32_t Rand() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

constexpr int kMax = 4;
constexpr uint32_t kTicks = 3600;   // a minute at 60
constexpr uint32_t kSlots = 64;
uint32_t g_lat[kMax][kMax];         // one-way ms
bool g_uplink_only = false;         // the laggy machine's sends are slow, what it receives is fast
uint32_t g_jitter_ms = 0, g_loss = 0;
int g_n = 0;

struct Packet { uint64_t at; int to; std::vector<uint8_t> data; };
std::deque<Packet> g_wire;

void SendFrom(void *user, const uint8_t *data, uint32_t size) {
    const int from = (int)(intptr_t)user;
    for (int to = 0; to < g_n; to++) {
        if (to == from || Rand() % 100 < g_loss) continue;
        const uint64_t at = g_now + g_lat[from][to] * 1000ull + (g_jitter_ms ? Rand() % (g_jitter_ms * 1000) : 0);
        g_wire.push_back({ at, to, std::vector<uint8_t>(data, data + size) });
    }
}

n64net_pad PadOf(int player, uint32_t tick) {
    n64net_pad p = {};
    uint32_t h = (uint32_t)player * 2654435761u ^ (tick / 9) * 40503u;
    p.buttons = (uint16_t)(h & 0xF0F0);
    p.stick_x = (int8_t)(h >> 16);
    return p;
}
uint64_t Mix(uint64_t s, uint64_t v) { return (s ^ v) * 1099511628211ull + 0x9E37; }

struct M {
    n64net_input *net = nullptr;
    uint32_t tick = 0;
    bool sampled = false;
    uint64_t state = 0;
    std::vector<uint64_t> hist = std::vector<uint64_t>(kTicks + 64);
    uint64_t ring[kSlots + 1] = {};   // + the anchor (deep-window runs)
    uint32_t frames = 0, stalls = 0, skips = 0, resim = 0;
    uint32_t max_delay = 0;
    bool in_resim = false;
} g_m[kMax];

void Step(M &m) {
    for (int p = 0; p < g_n; p++) {
        n64net_pad pad;
        n64net_input_get(m.net, p, m.tick, &pad);
        m.state = Mix(m.state, (uint64_t)pad.buttons << 8 | (uint8_t)pad.stick_x);
    }
    m.hist[m.tick] = m.state;
    m.tick++;
}
void Save(void *u, uint32_t slot) { ((M *)u)->ring[slot] = ((M *)u)->state; }
void Load(void *u, uint32_t slot) {
    M &m = *(M *)u;
    m.state = m.ring[slot];
    m.tick = n64net_rollback_callback_tick(m.net);
}
void Advance(void *u) { M &m = *(M *)u; m.resim++; Step(m); }
void Swap(void *u, uint32_t a, uint32_t b) { M &m = *(M *)u; std::swap(m.ring[a], m.ring[b]); }
uint32_t g_slots = kSlots;     // ring size for the rollback driver
bool g_swap = false;           // give it the anchor (swap)
uint32_t g_max_delay_rb = 3;   // auto delay ceiling with rollback

struct Result { uint32_t stalls, skips, resim, frames, delay; bool same; };

bool Run(const char *name, int n, int laggy, uint32_t fast_ms, uint32_t slow_ms, uint32_t window,
         Result out[kMax]) {
    g_now = 0;
    g_rng = 4242;
    g_wire.clear();
    g_n = n;
    for (int a = 0; a < n; a++)
        for (int b = 0; b < n; b++)
            g_lat[a][b] = (a == laggy || (b == laggy && !g_uplink_only)) ? slow_ms : fast_ms;
    for (int i = 0; i < n; i++) {
        g_m[i] = M();
        n64net_input_config c = {};
        c.num_players = n;
        c.local_player = i;
        c.session_id = 5;
        c.input_delay = 1;
        c.rollback_window = window;
        c.ticks_per_second = 60;
        c.send = SendFrom;
        c.user = (void *)(intptr_t)i;
        c.now_usecs = Now;
        g_m[i].net = n64net_input_create(&c);
        n64net_input_set_auto_delay(g_m[i].net, 1, window ? g_max_delay_rb : 6);
    }
    const n64net_rollback_callbacks cb0 = { Save, Load, Advance, nullptr, g_slots, g_swap ? Swap : nullptr };
    uint64_t next_frame[kMax];
    for (int i = 0; i < n; i++) next_frame[i] = (uint64_t)i * 4100;   // frame clocks out of phase
    for (uint64_t step = 0; step < 4000000; step++) {
        bool done = true;
        for (int i = 0; i < n; i++) done &= g_m[i].tick >= kTicks;
        if (done) break;
        g_now += 250;
        for (auto it = g_wire.begin(); it != g_wire.end();) {
            if (it->at <= g_now) {
                n64net_input_handle_packet(g_m[it->to].net, it->data.data(), (uint32_t)it->data.size());
                it = g_wire.erase(it);
            } else {
                ++it;
            }
        }
        for (int i = 0; i < n; i++) {
            M &m = g_m[i];
            if (m.tick >= kTicks || g_now < next_frame[i]) continue;
            next_frame[i] += 16667;   // one displayed frame
            m.frames++;
            if (window && n64net_timesync_should_skip(m.net)) { m.skips++; continue; }
            if (!m.sampled) {
                n64net_rollback_callbacks cb = cb0;
                cb.user = &m;
                if (window) n64net_rollback_frame(m.net, m.tick, &cb);
                n64net_input_set_local(m.net, m.tick, PadOf(i, m.tick));
                m.sampled = true;
                m.max_delay = std::max(m.max_delay, n64net_input_get_delay(m.net));
            }
            if (!n64net_input_ready(m.net, m.tick)) {
                n64net_input_resend(m.net);
                if (m.tick > 300) m.stalls++;   // after the first 5 s (round trips measured, delays settled)
                continue;
            }
            Step(m);
            m.sampled = false;
        }
    }
    // settle: deliver everything, resimulate
    for (int r = 0; r < 50 && !g_wire.empty(); r++) {
        g_now += 100000;
        auto w = g_wire;
        g_wire.clear();
        for (auto &pk : w) n64net_input_handle_packet(g_m[pk.to].net, pk.data.data(), (uint32_t)pk.data.size());
        for (int i = 0; i < n; i++) n64net_input_resend(g_m[i].net);
    }
    for (int i = 0; i < n; i++) {
        if (!window) continue;
        n64net_rollback_callbacks cb = cb0;
        cb.user = &g_m[i];
        n64net_rollback_frame(g_m[i].net, g_m[i].tick, &cb);
    }
    uint32_t confirmed = kTicks - 1;
    for (int i = 0; i < n; i++) {
        uint32_t c = n64net_input_confirmed_tick(g_m[i].net);
        confirmed = std::min(confirmed, c == UINT32_MAX ? 0 : std::min(c, g_m[i].tick - 1));
    }
    bool same = confirmed > kTicks - 100;
    for (uint32_t t = 0; t <= confirmed && same; t++)
        for (int i = 1; i < n; i++) same &= g_m[i].hist[t] == g_m[0].hist[t];
    printf("%s (%s, fast links %u ms, %s %u ms):%s\n", name, window ? "rollback" : "lockstep", fast_ms,
           laggy >= 0 ? "laggy machine's links" : "", slow_ms, same ? "" : "  STATES DIFFER");
    for (int i = 0; i < n; i++) {
        M &m = g_m[i];
        n64net_input_stats st;
        n64net_input_get_stats(m.net, &st);
        out[i] = { m.stalls, m.skips, m.resim, m.frames, m.max_delay, same };
        printf("  machine %d%s: delay up to %2u, stalls %4u, time-sync skips %3u, resimulated ticks/frame %.2f\n", i,
               i == laggy ? " (laggy)" : "        ", m.max_delay, m.stalls, m.skips, (double)m.resim / m.frames);
        n64net_input_destroy(m.net);
    }
    return same;
}

} // namespace

int main() {
    bool ok = true;
    Result r[kMax];
    auto check = [&](const char *what, bool c) { printf("  %s  %s\n", c ? "PASS" : "FAIL", what); ok &= c; };
    g_jitter_ms = 10;
    g_loss = 1;

    // 4 machines, one 180 ms away from everyone (a ~360 ms round trip), the rest 15 ms apart
    ok &= Run("4 machines, one laggy", 4, 3, 15, 180, 8, r);
    {
        uint32_t worst_stall = 0, worst_skip = 0; double worst_resim = 0;
        for (int i = 0; i < 3; i++) {
            worst_stall = std::max(worst_stall, r[i].stalls);
            worst_skip = std::max(worst_skip, r[i].skips);
            worst_resim = std::max(worst_resim, (double)r[i].resim / r[i].frames);
        }
        check("the normal machines keep a normal delay (<= 3)", r[0].delay <= 3 && r[1].delay <= 3 && r[2].delay <= 3);
        check("the normal machines don't stall", worst_stall <= 30);
        check("the normal machines roll back little (< 1.5 ticks per frame)", worst_resim < 1.5);
        check("the laggy machine takes the delay", r[3].delay > 3);
    }
    ok &= Run("3 machines, one laggy", 3, 2, 20, 250, 8, r);
    check("normal machines: delay <= 3, few stalls", r[0].delay <= 3 && r[1].delay <= 3 && r[0].stalls <= 30 &&
                                                      r[1].stalls <= 30);
    ok &= Run("3 machines, one laggy", 3, 2, 20, 150, 0, r);
    check("lockstep: the normal machines keep a normal delay (<= 6)", r[0].delay <= 6 && r[1].delay <= 6);
    check("lockstep: the normal machines stall little", r[0].stalls <= 60 && r[1].stalls <= 60);
    g_uplink_only = true;   // a bad uplink (e.g. a phone on a busy cell): only its sends are slow
    ok &= Run("3 machines, one with a slow uplink (300 ms)", 3, 2, 15, 300, 8, r);
    check("uplink: the normal machines don't stall, roll back little", r[0].stalls <= 30 && r[1].stalls <= 30 &&
                                                                       (double)r[0].resim / r[0].frames < 1.5);
    check("uplink: the laggy machine doesn't stall either", r[2].stalls <= 60);
    g_uplink_only = false;
    // a deep window (40) with a 12-slot ring and the anchor: spikes are guessed through, nothing waits
    g_slots = 12;
    g_swap = true;
    g_jitter_ms = 600;   // spiky: inputs up to ~0.6 s late now and then
    g_loss = 5;
    ok &= Run("3 machines, spiky network, deep window + anchor", 3, -1, 60, 60, 40, r);
    check("deep window: states identical, few stalls", r[0].same && r[0].stalls <= 30 && r[1].stalls <= 30 &&
                                                          r[2].stalls <= 30);
    g_slots = kSlots;
    g_swap = false;
    ok &= Run("3 machines, spiky network, window 8 (for comparison)", 3, -1, 60, 60, 8, r);
    g_jitter_ms = 10;
    g_loss = 1;
    ok &= Run("2 machines on a slow link", 2, -1, 150, 150, 8, r);
    check("2 machines: the same delay on both sides", r[0].delay == r[1].delay);
    ok &= Run("4 machines, all normal", 4, -1, 25, 25, 8, r);
    check("all normal: delays <= 3, no laggy one", r[0].delay <= 3 && r[3].delay <= 3);
    printf("%s\n", ok ? "ALL PASS" : "FAILED");
    return ok ? 0 : 1;
}
