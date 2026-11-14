// Rendezvous client against a local server: a room for up to 4 with a host and two guests (threads), each on
// its own UDP socket. Start the server first:  python3 server/rendezvous.py --port 7798 --bind 127.0.0.1
//   clang++ -std=c++17 -Iclient/include client/src/n64net_rendezvous.cpp client/test/rendezvous_test.cpp -o rv_test
#include "n64net_rendezvous.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
int MakeSock() {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, (sockaddr *)&a, sizeof(a));
    fcntl(s, F_SETFL, O_NONBLOCK);
    return s;
}

struct Side {
    int sock = -1;
    bool host = false;
    int rc = -99;
    N64NetRendezvousResult res{};
};

void Connect(Side *s) {
    N64NetRendezvousConfig c{};
    c.server = "127.0.0.1:7798";
    c.game = "test+1";
    c.room = "QQQQ";
    c.is_host = s->host ? 1 : 0;
    c.wait_secs = 10;
    c.max_members = 4;
    s->rc = n64net_rendezvous_connect(s->sock, &c, &s->res);
}
} // namespace

int main() {
    Side h, g1, g2;
    h.host = true;
    h.sock = MakeSock();
    g1.sock = MakeSock();
    g2.sock = MakeSock();
    std::thread th(Connect, &h);
    usleep(300000);
    std::thread t1(Connect, &g1);
    th.join();
    t1.join();
    std::thread t2(Connect, &g2);
    // the host and guest 1 read their sockets for the MEMBER messages about guest 2
    int host_saw = -1, g1_saw = -1;
    for (int i = 0; i < 300 && (host_saw < 0 || g1_saw < 0); i++) {
        for (Side *s : { &h, &g1 }) {
            char buf[512];
            ssize_t n = recv(s->sock, buf, sizeof(buf), 0);
            if (n <= 0) continue;
            char room[8];
            int idx = -1, mine = -1;
            sockaddr_in pub{}, lan{};
            char tok[24];
            if (n64net_rendezvous_parse_member(buf, (int)n, room, &idx, &pub, &lan, &mine, tok, sizeof(tok)) == 1 && idx == 2) {
                (s == &h ? host_saw : g1_saw) = mine;
            }
        }
        usleep(10000);
    }
    t2.join();
    bool ok = h.rc == 0 && g1.rc == 0 && g2.rc == 0;
    ok &= h.res.member_index == 0 && g1.res.member_index == 1 && g2.res.member_index == 2;
    ok &= g1.res.peer_index == 0 && g2.res.peer_index == 0 && h.res.peer_index == 1;
    ok &= host_saw == 0 && g1_saw == 1;
    printf("connect rc %d/%d/%d, members %d/%d/%d (paired with %d/%d/%d), later MEMBER 2 seen by host %d guest1 %d -> %s\n",
           h.rc, g1.rc, g2.rc, h.res.member_index, g1.res.member_index, g2.res.member_index, h.res.peer_index,
           g1.res.peer_index, g2.res.peer_index, host_saw, g1_saw, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
