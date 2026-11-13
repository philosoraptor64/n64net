/* n64net rendezvous client. See n64net_rendezvous.h. */
#include "n64net_rendezvous.h"
#include "n64net_hmac.h"

#if !defined(_WIN32)

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char *kPrefix = "N64R1 ";
constexpr size_t kPrefixLen = 6;
constexpr int kRegisterIntervalMs = 1000;
constexpr int kPunchIntervalMs = 100;
constexpr int kPunchTimeoutMs = 5000;   // then fall back to the relay
constexpr int kPunchLingerMs = 1000;    // keep answering (and acking) after success
constexpr int kRelayRequestIntervalMs = 300;
constexpr int kRelayTimeoutMs = 5000;

using Clock = std::chrono::steady_clock;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

void NoLog(const char *, ...) {
}

std::string AddrStr(const sockaddr_in &a) {
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

bool SameAddr(const sockaddr_in &a, const sockaddr_in &b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

// "host:port" -> address (DNS names allowed).
bool Resolve(const std::string &text, sockaddr_in *out) {
    size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0) {
        return false;
    }
    std::string host = text.substr(0, colon);
    std::string port = text.substr(colon + 1);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || res == nullptr) {
        return false;
    }
    std::memcpy(out, res->ai_addr, sizeof(sockaddr_in));
    freeaddrinfo(res);
    return true;
}

// Our LAN address as the route to the server sees it, with the game
// socket's port: lets two players behind the same router connect locally.
std::string LanAddr(int sock, const sockaddr_in &server) {
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    getsockname(sock, (sockaddr *)&bound, &len);

    sockaddr_in local{};
    int probe = socket(AF_INET, SOCK_DGRAM, 0);
    if (probe >= 0) {
        if (connect(probe, (const sockaddr *)&server, sizeof(server)) == 0) {
            len = sizeof(local);
            getsockname(probe, (sockaddr *)&local, &len);
        }
        close(probe);
    }
    local.sin_port = bound.sin_port;
    return AddrStr(local);
}

class Session {
  public:
    Session(int sock, const N64NetRendezvousConfig &cfg) : mSock(sock), mCfg(cfg) {
        mLog = cfg.log ? cfg.log : NoLog;
        mIsHost = cfg.is_host != 0;
        if (cfg.password != nullptr) {
            mPassword = cfg.password;
        }
        if (cfg.invite_pass != nullptr) {
            mInvitePass = cfg.invite_pass;
        }
        if (cfg.room != nullptr) {
            for (const char *c = cfg.room; *c && mRoom.size() < 4; c++) {
                mRoom += (char)toupper((unsigned char)*c);
            }
        }
    }

    int Run(N64NetRendezvousResult *out);

  private:
    void Send(const sockaddr_in &to, const std::string &msg) {
        std::string pkt = kPrefix + msg;
        sendto(mSock, pkt.data(), pkt.size(), 0, (const sockaddr *)&to, sizeof(to));
    }
    // Server commands carry "<unix_time> <mac>" when a password is set; the
    // password itself is never sent (see rendezvous.py).
    void SendToServer(const std::string &msg) {
        if (mPassword.empty()) {
            // A guest with an invite pass signs JOIN/RELAY/BYE with it.
            Send(mServer, mInvitePass.empty() || mIsHost ? msg : msg + " pass " + mInvitePass);
            return;
        }
        std::string signed_msg = msg + " " + std::to_string((long long)std::time(nullptr));
        Send(mServer, signed_msg + " " + n64net::HmacSha256Hex(mPassword, signed_msg).substr(0, 32));
    }
    void Register() {
        if (mIsHost) {
            SendToServer("HOST " + std::string(mCfg.game) + " " + (mRoom.empty() ? "-" : mRoom) + " " + mLan);
        } else {
            SendToServer("JOIN " + std::string(mCfg.game) + " " + mRoom + " " + mLan);
        }
    }
    // Waits up to `ms` for one datagram. Returns its size, 0 if none, -1 on error.
    int Receive(int ms, char *buf, size_t cap, sockaddr_in *from) {
        if (mCfg.yield) {
            mCfg.yield();
        }
        pollfd p{ mSock, POLLIN, 0 };
        if (poll(&p, 1, ms) <= 0) {
            return 0;
        }
        socklen_t len = sizeof(*from);
        ssize_t n = recvfrom(mSock, buf, cap - 1, 0, (sockaddr *)from, &len);
        if (n < 0) {
            return 0;
        }
        buf[n] = '\0';
        return (int)n;
    }
    static bool IsControl(const char *buf, int n) {
        return n >= (int)kPrefixLen && std::memcmp(buf, kPrefix, kPrefixLen) == 0;
    }
    static std::vector<std::string> Split(const char *s) {
        std::vector<std::string> out;
        std::string cur;
        for (; *s; s++) {
            if (*s == ' ' || *s == '\n' || *s == '\r') {
                if (!cur.empty()) {
                    out.push_back(cur);
                    cur.clear();
                }
            } else {
                cur += *s;
            }
        }
        if (!cur.empty()) {
            out.push_back(cur);
        }
        return out;
    }

    bool WaitForPeer();
    bool Punch();
    bool RequestRelay();

    int mSock;
    const N64NetRendezvousConfig &mCfg;
    void (*mLog)(const char *, ...);
    bool mIsHost;
    std::string mRoom;
    std::string mPassword;
    sockaddr_in mServer{};
    std::string mInvitePass;
    std::string mLan;
    std::vector<sockaddr_in> mCandidates;
    std::string mToken;   // PEER's 6th field: this side's move token
    sockaddr_in mPeer{};
    std::string mError;
};

bool Session::WaitForPeer() {
    const int64_t deadline = NowMs() + (int64_t)(mCfg.wait_secs > 0 ? mCfg.wait_secs : 600) * 1000;
    int64_t next_register = 0;
    bool announced = false;
    bool room_missing = false;
    char buf[1600];

    while (NowMs() < deadline) {
        if (mCfg.cancel && *mCfg.cancel) {
            mLog("n64net: stopped waiting (cancelled)\n");
            mError = "cancelled";
            return false;
        }
        if (NowMs() >= next_register) {
            Register();
            next_register = NowMs() + kRegisterIntervalMs;
        }
        sockaddr_in from{};
        int n = Receive(20, buf, sizeof(buf), &from);
        if (n <= 0 || !IsControl(buf, n) || !SameAddr(from, mServer)) {
            continue;
        }
        auto f = Split(buf + kPrefixLen);
        if (f.empty()) {
            continue;
        }
        if (f[0] == "ROOM" && f.size() >= 2) {
            if (!announced || mRoom != f[1]) {
                mRoom = f[1];
                mLog("n64net: room code %s - waiting for the other player\n", mRoom.c_str());
                announced = true;
            }
        } else if (f[0] == "PEER" && f.size() >= 5) {
            sockaddr_in pub{}, lan{};
            if (!Resolve(f[2], &pub)) {
                continue;
            }
            mCandidates.clear();
            mCandidates.push_back(pub);
            if (f.size() >= 6) {
                mToken = f[5];
            }
            if (Resolve(f[3], &lan) && !SameAddr(lan, pub) && lan.sin_addr.s_addr != 0) {
                mCandidates.push_back(lan);
            }
            mLog("n64net: room %s paired with %s (public %s, lan %s)\n", f[1].c_str(), f[4].c_str(), f[2].c_str(),
                 f[3].c_str());
            return true;
        } else if (f[0] == "ERR" && f.size() >= 2) {
            if (f[1] == "nosuchroom" && !mIsHost) {
                room_missing = true;
                if (!announced) {
                    mLog("n64net: room %s not open yet - still trying\n", mRoom.c_str());
                    announced = true;
                }
                continue;
            }
            if (f[1] == "auth") {
                mLog("n64net: server refused the password (check the password and the clock)\n");
            } else if (f[1] == "version") {
                mLog("n64net: the host runs a different build (%s); both players need the same version\n",
                     f.size() >= 3 ? f[2].c_str() : "?");
            } else {
                mLog("n64net: server refused: %s\n", f[1].c_str());
            }
            mError = f[1] + (f[1] == "version" && f.size() >= 3 ? " " + f[2] : "");
            return false;
        }
    }
    mLog("n64net: gave up waiting for the other player\n");
    mError = room_missing ? "noroom" : "timeout";
    return false;
}

bool Session::Punch() {
    const std::string role = mIsHost ? "host" : "guest";
    const int64_t start = NowMs();
    int64_t next_punch = 0, next_register = 0, linger_until = 0;
    bool connected = false;
    char buf[1600];

    while (true) {
        int64_t now = NowMs();
        if (connected && now >= linger_until) {
            return true;
        }
        if (!connected && now - start >= kPunchTimeoutMs) {
            return false;
        }
        if (now >= next_punch) {
            if (!connected) {
                for (auto &c : mCandidates) {
                    Send(c, "PUNCH " + mRoom + " " + role);
                }
            } else {
                // Connected: keep acking on the working path for a moment, so
                // a peer whose own PUNCHes never got through still hears us
                // and goes direct too (instead of falling back to the relay).
                Send(mPeer, "PUNCHACK " + mRoom + " " + role);
            }
            next_punch = now + kPunchIntervalMs;
        }
        if (now >= next_register) {
            Register();  // keeps the room and our server-side NAT mapping alive
            next_register = now + kRegisterIntervalMs;
        }
        sockaddr_in from{};
        int n = Receive(10, buf, sizeof(buf), &from);
        if (n <= 0 || SameAddr(from, mServer)) {
            continue;
        }
        bool from_candidate = false;
        for (auto &c : mCandidates) {
            from_candidate |= SameAddr(c, from);
        }
        if (!IsControl(buf, n)) {
            // A gameplay packet from the peer: the direct path already works.
            if (from_candidate && !connected) {
                mPeer = from;
                connected = true;
                linger_until = NowMs() + kPunchLingerMs;
            }
            continue;
        }
        auto f = Split(buf + kPrefixLen);
        if (f.size() < 2 || f[1] != mRoom) {
            continue;
        }
        if (f[0] == "PUNCH") {
            // Answer from wherever it came: that is the hole the peer opened.
            Send(from, "PUNCHACK " + mRoom + " " + role);
            // ...and dial it ourselves too: the address the server reported
            // can differ from the one the peer's packets really come from
            // (a NAT that rewrites ports, or a host registered via 127.0.0.1).
            if (!from_candidate) {
                mCandidates.push_back(from);
            }
        } else if (f[0] == "PUNCHACK" && !connected) {
            mPeer = from;
            connected = true;
            linger_until = NowMs() + kPunchLingerMs;
            mLog("n64net: direct connection to %s after %lld ms\n", AddrStr(from).c_str(),
                 (long long)(NowMs() - start));
        }
    }
}

bool Session::RequestRelay() {
    const int64_t start = NowMs();
    int64_t next_request = 0;
    char buf[1600];

    while (NowMs() - start < kRelayTimeoutMs) {
        if (NowMs() >= next_request) {
            SendToServer("RELAY " + mRoom);
            next_request = NowMs() + kRelayRequestIntervalMs;
        }
        sockaddr_in from{};
        int n = Receive(20, buf, sizeof(buf), &from);
        if (n <= 0 || !IsControl(buf, n)) {
            continue;
        }
        auto f = Split(buf + kPrefixLen);
        if (f.size() >= 2 && f[0] == "PUNCHACK" && f[1] == mRoom) {
            continue;  // too late; stay on the relay so both sides agree
        }
        if (f.size() >= 2 && f[0] == "PUNCH" && f[1] == mRoom) {
            Send(from, "PUNCHACK " + mRoom + (mIsHost ? " host" : " guest"));
            continue;
        }
        if (SameAddr(from, mServer) && f.size() >= 2 && f[0] == "RELAYOK") {
            mPeer = mServer;
            mLog("n64net: using the server relay\n");
            return true;
        }
    }
    mLog("n64net: relay request timed out\n");
    mError = "relay";
    return false;
}

int Session::Run(N64NetRendezvousResult *out) {
    if (mCfg.server == nullptr || mCfg.game == nullptr || !Resolve(mCfg.server, &mServer)) {
        mLog("n64net: cannot resolve server '%s'\n", mCfg.server ? mCfg.server : "(null)");
        std::snprintf(out->error, sizeof(out->error), "resolve");
        return -1;
    }
    if (!mIsHost && mRoom.size() != 4) {
        mLog("n64net: joining needs a 4-character room code\n");
        std::snprintf(out->error, sizeof(out->error), "args");
        return -1;
    }
    mLan = LanAddr(mSock, mServer);
    mLog("n64net: %s room %s via %s (lan %s)\n", mIsHost ? "hosting" : "joining",
         mRoom.empty() ? "(random)" : mRoom.c_str(), AddrStr(mServer).c_str(), mLan.c_str());

    if (!WaitForPeer()) {
        std::snprintf(out->error, sizeof(out->error), "%s", mError.c_str());
        return -2;
    }
    bool relayed = false;
    if (mCfg.force_relay || !Punch()) {
        if (!mCfg.force_relay) {
            mLog("n64net: no direct path after %d ms; asking the server to relay\n", kPunchTimeoutMs);
        }
        if (!RequestRelay()) {
            std::snprintf(out->error, sizeof(out->error), "%s", mError.c_str());
            return -3;
        }
        relayed = true;
    }
    out->peer = mPeer;
    out->server = mServer;
    out->relayed = relayed ? 1 : 0;
    out->is_host = mIsHost ? 1 : 0;
    std::snprintf(out->room, sizeof(out->room), "%s", mRoom.c_str());
    std::snprintf(out->token, sizeof(out->token), "%s", mToken.c_str());
    return 0;
}

} // namespace

extern "C" void n64net_rendezvous_bye(int sock, const N64NetRendezvousConfig *config) {
    sockaddr_in server{};
    if (sock < 0 || config == nullptr || config->server == nullptr || config->room == nullptr ||
        !Resolve(config->server, &server)) {
        return;
    }
    std::string room;
    for (const char *c = config->room; *c && room.size() < 4; c++) {
        room += (char)toupper((unsigned char)*c);
    }
    std::string msg = "BYE " + room;
    if (config->password != nullptr && config->password[0] != '\0') {
        msg += " " + std::to_string((long long)std::time(nullptr));
        msg += " " + n64net::HmacSha256Hex(config->password, msg).substr(0, 32);
    } else if (config->invite_pass != nullptr && config->invite_pass[0] != '\0') {
        msg += std::string(" pass ") + config->invite_pass;
    }
    const std::string pkt = kPrefix + msg;
    for (int i = 0; i < 3; i++) {
        sendto(sock, pkt.data(), pkt.size(), 0, (const sockaddr *)&server, sizeof(server));
    }
}

extern "C" int n64net_rendezvous_connect(int sock, const N64NetRendezvousConfig *config,
                                         N64NetRendezvousResult *out) {
    if (sock < 0 || config == nullptr || out == nullptr) {
        return -1;
    }
    std::memset(out, 0, sizeof(*out));
    Session session(sock, *config);
    return session.Run(out);
}

#else

extern "C" int n64net_rendezvous_connect(int, const N64NetRendezvousConfig *, N64NetRendezvousResult *) {
    return -1;
}

extern "C" void n64net_rendezvous_bye(int, const N64NetRendezvousConfig *) {
}

#endif

extern "C" int n64net_rendezvous_request_invite(int sock, const N64NetRendezvousConfig *config, int ttl_secs) {
    sockaddr_in server{};
    if (sock < 0 || config == nullptr || config->server == nullptr || config->room == nullptr ||
        config->password == nullptr || config->password[0] == '\0' || !Resolve(config->server, &server)) {
        return -1;
    }
    std::string room;
    for (const char *c = config->room; *c && room.size() < 4; c++) {
        room += (char)toupper((unsigned char)*c);
    }
    std::string msg = "INVITE " + room + " " + std::to_string(ttl_secs > 0 ? ttl_secs : 24 * 3600);
    msg += " " + std::to_string((long long)std::time(nullptr));
    msg += " " + n64net::HmacSha256Hex(config->password, msg).substr(0, 32);
    const std::string pkt = kPrefix + msg;
    return sendto(sock, pkt.data(), pkt.size(), 0, (const sockaddr *)&server, sizeof(server)) > 0 ? 0 : -1;
}

extern "C" int n64net_rendezvous_parse_invite(const void *data, int size, char room_out[8], char *pass_out,
                                              int pass_cap) {
    if (data == nullptr || size <= (int)kPrefixLen || size > 200 || pass_out == nullptr || pass_cap <= 0) {
        return 0;
    }
    const char *p = static_cast<const char *>(data);
    if (std::memcmp(p, kPrefix, kPrefixLen) != 0) {
        return 0;
    }
    std::vector<std::string> f;
    {
        std::string word;
        for (int i = (int)kPrefixLen; i < size; i++) {
            const char c = p[i];
            if (c == ' ' || c == '\n' || c == '\r' || c == '\0') {
                if (!word.empty()) f.push_back(word);
                word.clear();
            } else {
                word += c;
            }
        }
        if (!word.empty()) f.push_back(word);
    }
    if (f.size() != 3 || f[0] != "INVITE" || (int)f[2].size() >= pass_cap) {
        return 0;
    }
    if (room_out != nullptr) {
        std::snprintf(room_out, 8, "%s", f[1].c_str());
    }
    std::snprintf(pass_out, pass_cap, "%s", f[2].c_str());
    return 1;
}
