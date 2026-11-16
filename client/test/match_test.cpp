// Matchmaking end to end against a local server with a password: two devices look for a match, get paired, and
// connect through the usual rendezvous (room for up to 4). Also presence, LIST and names with spaces/UTF-8.
// Start the server first:  echo -n testpw > /tmp/pw && python3 server/rendezvous.py --port 7795 --bind 127.0.0.1 --password-file /tmp/pw
//   clang++ -std=c++17 -Iclient/include client/src/n64net_match.cpp client/src/n64net_rendezvous.cpp \
//       client/test/match_test.cpp -o match_test
#include "n64net_match.h"
#include "n64net_rendezvous.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
const char *kServer = "127.0.0.1:7795";
const char *kPassword = "testpw";

int MakeSock(std::string *lan) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, (sockaddr *)&a, sizeof(a));
    socklen_t n = sizeof(a);
    getsockname(s, (sockaddr *)&a, &n);
    *lan = "127.0.0.1:" + std::to_string(ntohs(a.sin_port));
    fcntl(s, F_SETFL, O_NONBLOCK);
    return s;
}

// waits up to `ms` for a matchmaking answer of `kind`
bool Await(int sock, int kind, N64NetMatchEvent *ev, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    char buf[2000];
    while (std::chrono::steady_clock::now() < end) {
        pollfd p{ sock, POLLIN, 0 };
        if (poll(&p, 1, 20) <= 0) continue;
        int n = (int)recv(sock, buf, sizeof(buf), 0);
        if (n > 0 && n64net_match_parse(buf, n, ev) == kind) return true;
    }
    return false;
}

struct Dev {
    std::string name, games;
    char device[17] = {};
    int sock = -1;
    std::string lan;
    N64NetMatchEvent match{};
    bool matched = false;
    int rc = -99;
    N64NetRendezvousResult res{};
};

void Seek(Dev *d) {
    N64NetMatchConfig c{ kServer, kPassword, d->device, d->name.c_str() };
    char buf[2000];
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto next = std::chrono::steady_clock::now();
    while (!d->matched && std::chrono::steady_clock::now() < end) {
        if (std::chrono::steady_clock::now() >= next) {   // repeat ~every 300 ms in the test (3 s in the app)
            n64net_match_seek(d->sock, &c, "i64-session-1", d->games.c_str(), d->lan.c_str());
            next += std::chrono::milliseconds(300);
        }
        pollfd p{ d->sock, POLLIN, 0 };
        if (poll(&p, 1, 20) <= 0) continue;
        int n = (int)recv(d->sock, buf, sizeof(buf), 0);
        if (n > 0 && n64net_match_parse(buf, n, &d->match) == N64NET_MATCH_FOUND) d->matched = true;
    }
    if (!d->matched) return;
    // the usual rendezvous into the matched room
    N64NetRendezvousConfig rc{};
    rc.server = kServer;
    rc.game = "i64-session-1";
    rc.room = d->match.code;
    rc.password = kPassword;
    rc.is_host = d->match.is_host;
    rc.force_relay = d->match.relay;
    rc.wait_secs = 10;
    rc.max_members = 4;
    if (!rc.is_host) std::this_thread::sleep_for(std::chrono::milliseconds(200));   // the host opens the room first
    d->rc = n64net_rendezvous_connect(d->sock, &rc, &d->res);
}
} // namespace

int main() {
    bool ok = true;
    auto check = [&](const char *what, bool cond) {
        std::printf("%s  %s\n", cond ? "PASS" : "FAIL", what);
        ok = ok && cond;
    };

    Dev a, b;
    a.name = "Zoë B";
    a.games = "mk64-us+aaa,ssb64-us+bbb";
    b.name = "Max";
    b.games = "pd-us+ccc,ssb64-us+bbb";
    n64net_match_new_device(a.device);
    n64net_match_new_device(b.device);
    check("device ids are 16 hex digits and differ", std::strlen(a.device) == 16 && std::strcmp(a.device, b.device));
    a.sock = MakeSock(&a.lan);
    b.sock = MakeSock(&b.lan);

    // presence + LIST: B sees A's name decoded
    N64NetMatchConfig ca{ kServer, kPassword, a.device, a.name.c_str() };
    N64NetMatchConfig cb{ kServer, kPassword, b.device, b.name.c_str() };
    N64NetMatchEvent ev{};
    n64net_match_presence(a.sock, &ca, "lobby", "mk64-us");
    check("presence answered", Await(a.sock, N64NET_MATCH_PRESOK, &ev, 1000) && ev.online >= 1);
    n64net_match_presence(b.sock, &cb, "lobby", "-");
    Await(b.sock, N64NET_MATCH_PRESOK, &ev, 1000);
    n64net_match_list(b.sock, &cb, 0);
    bool listed = Await(b.sock, N64NET_MATCH_PEOPLE, &ev, 1000);
    bool sawA = false;
    for (int i = 0; listed && i < ev.count; i++) {
        sawA = sawA || (!std::strcmp(ev.people[i].name, "Zoë B") && !std::strcmp(ev.people[i].state, "lobby") &&
                        !std::strcmp(ev.people[i].info, "mk64-us"));
    }
    check("LIST shows the other device with its UTF-8 name", sawA);

    // both look for a match, then connect
    std::thread ta(Seek, &a), tb(Seek, &b);
    ta.join();
    tb.join();
    check("both got MATCH", a.matched && b.matched);
    check("same room code, one host and one guest",
          !std::strcmp(a.match.code, b.match.code) && a.match.is_host != b.match.is_host);
    check("the common game is the shared one", !std::strcmp(a.match.game, "ssb64-us+bbb") &&
                                                   !std::strcmp(b.match.game, "ssb64-us+bbb"));
    check("each learns the other's name", !std::strcmp(a.match.peer_name, "Max") &&
                                              !std::strcmp(b.match.peer_name, "Zoë B"));
    check("friends: no forced relay", a.match.relay == 0 && b.match.relay == 0);
    check("both connected through the matched room", a.rc == 0 && b.rc == 0);

    // unseek answers
    n64net_match_unseek(a.sock, &ca);
    check("unseek answered (-1)", Await(a.sock, N64NET_MATCH_SEEKING, &ev, 1000) && ev.others_seeking == -1);

    // wrong password: ERR auth
    N64NetMatchConfig bad{ kServer, "nope", b.device, "x" };
    n64net_match_presence(b.sock, &bad, "lobby", "-");
    check("wrong password refused", Await(b.sock, N64NET_MATCH_ERROR, &ev, 1000) && !std::strcmp(ev.error, "auth"));

    close(a.sock);
    close(b.sock);
    std::printf("%s\n", ok ? "ALL PASS" : "FAILED");
    return ok ? 0 : 1;
}
