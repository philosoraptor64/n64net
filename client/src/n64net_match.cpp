// n64net matchmaking client (see n64net_match.h and server/rendezvous.py "Matchmaking").
#include "n64net_match.h"
#include "n64net_hmac.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <vector>

namespace {

const char kPrefix[] = "N64R1 ";
constexpr size_t kPrefixLen = 6;
const char kVersion[] = "M1";

bool Resolve(const char *hostport, sockaddr_in *out) {
    if (hostport == nullptr) return false;
    std::string hp(hostport);
    size_t colon = hp.rfind(':');
    if (colon == std::string::npos) return false;
    std::string host = hp.substr(0, colon), port = hp.substr(colon + 1);
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || res == nullptr) return false;
    std::memcpy(out, res->ai_addr, sizeof(*out));
    freeaddrinfo(res);
    return true;
}

// Names travel percent-encoded (no spaces, commas or non-ASCII bytes on the wire).
std::string Encode(const char *s, size_t limit) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p && out.size() + 3 <= limit; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out += (char)*p;
        } else {
            out += '%';
            out += hex[*p >> 4];
            out += hex[*p & 15];
        }
    }
    return out.empty() ? "-" : out;
}

void Decode(const std::string &s, char *out, size_t cap) {
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && isxdigit((unsigned char)s[i + 1]) &&
            isxdigit((unsigned char)s[i + 2])) {
            r += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            r += s[i];
        }
    }
    std::snprintf(out, cap, "%s", r.c_str());
}

// A word for the wire: no spaces or commas, at most `limit` bytes, "-" if empty.
std::string Word(const char *s, size_t limit) {
    std::string out;
    for (const char *p = s ? s : ""; *p && out.size() < limit; p++) {
        if (*p > 32 && *p < 127 && *p != ',') out += *p;
    }
    return out.empty() ? "-" : out;
}

int SendSigned(int sock, const N64NetMatchConfig *c, const std::string &cmd_and_fields) {
    sockaddr_in server{};
    if (sock < 0 || c == nullptr || c->device == nullptr || !Resolve(c->server, &server)) return -1;
    std::string msg = cmd_and_fields;
    msg += " " + std::to_string((long long)std::time(nullptr));
    const std::string pw = c->password ? c->password : "";
    msg += " " + (pw.empty() ? std::string("0") : n64net::HmacSha256Hex(pw, msg).substr(0, 32));
    const std::string pkt = kPrefix + msg;
    return sendto(sock, pkt.data(), pkt.size(), 0, (const sockaddr *)&server, sizeof(server)) > 0 ? 0 : -1;
}

std::vector<std::string> Split(const std::string &s, char sep) {
    std::vector<std::string> f;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(sep, i);
        if (j == std::string::npos) j = s.size();
        if (j > i || sep == ',') f.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return f;
}

} // namespace

extern "C" void n64net_match_new_device(char out[17]) {
    std::random_device rd;
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) out[i] = hex[rd() & 15];
    out[16] = '\0';
}

extern "C" int n64net_match_presence(int sock, const N64NetMatchConfig *c, const char *state, const char *info) {
    if (c == nullptr) return -1;
    return SendSigned(sock, c, std::string("PRES ") + kVersion + " " + Word(c->device, 32) + " " +
                                   Encode(c->name, 96) + " " + Word(state, 10) + " " + Word(info, 64));
}

extern "C" int n64net_match_seek(int sock, const N64NetMatchConfig *c, const char *session, const char *games,
                                 const char *lan) {
    if (c == nullptr || games == nullptr || games[0] == '\0') return -1;
    std::string g;   // keep commas here: the list separator
    for (const char *p = games; *p && g.size() < 900; p++) {
        if (*p > 32 && *p < 127) g += *p;
    }
    return SendSigned(sock, c, std::string("SEEK ") + kVersion + " " + Word(c->device, 32) + " " +
                                   Encode(c->name, 96) + " " + Word(session, 64) + " " + g + " " +
                                   Word(lan, 64));
}

extern "C" int n64net_match_unseek(int sock, const N64NetMatchConfig *c) {
    if (c == nullptr) return -1;
    return SendSigned(sock, c, std::string("UNSEEK ") + kVersion + " " + Word(c->device, 32));
}

extern "C" int n64net_match_list(int sock, const N64NetMatchConfig *c, int page) {
    if (c == nullptr) return -1;
    return SendSigned(sock, c, std::string("LIST ") + kVersion + " " + Word(c->device, 32) + " " +
                                   std::to_string(page < 0 ? 0 : page));
}

extern "C" int n64net_match_open(int sock, const N64NetMatchConfig *c, const char *room_code, int open) {
    if (c == nullptr) return -1;
    return SendSigned(sock, c, std::string("OPEN ") + kVersion + " " + Word(c->device, 32) + " " +
                                   Word(room_code, 4) + " " + (open ? "1" : "0"));
}

extern "C" int n64net_match_call(int sock, const N64NetMatchConfig *c, const char *target_device,
                                 const char *session, const char *game) {
    if (c == nullptr || target_device == nullptr || target_device[0] == '\0') return -1;
    return SendSigned(sock, c, std::string("CALL ") + kVersion + " " + Word(c->device, 32) + " " +
                                   Encode(c->name, 96) + " " + Word(target_device, 32) + " " + Word(session, 64) +
                                   " " + Word(game, 80));
}

extern "C" int n64net_match_answer(int sock, const N64NetMatchConfig *c, const char *caller_device, int yes) {
    if (c == nullptr || caller_device == nullptr || caller_device[0] == '\0') return -1;
    return SendSigned(sock, c, std::string("ANSWER ") + kVersion + " " + Word(c->device, 32) + " " +
                                   Encode(c->name, 96) + " " + Word(caller_device, 32) + " " + (yes ? "yes" : "no"));
}

extern "C" int n64net_match_uncall(int sock, const N64NetMatchConfig *c) {
    if (c == nullptr) return -1;
    return SendSigned(sock, c, std::string("UNCALL ") + kVersion + " " + Word(c->device, 32));
}

extern "C" int n64net_match_parse(const void *data, int size, N64NetMatchEvent *out) {
    if (data == nullptr || out == nullptr || size <= (int)kPrefixLen || size > 2000 ||
        std::memcmp(data, kPrefix, kPrefixLen) != 0) {
        return N64NET_MATCH_NONE;
    }
    std::memset(out, 0, sizeof(*out));
    const std::vector<std::string> f =
        Split(std::string((const char *)data + kPrefixLen, (size_t)size - kPrefixLen), ' ');
    if (f.empty()) return N64NET_MATCH_NONE;
    const std::string &w = f[0];
    if (w == "ERR") {
        if (f.size() < 2) return N64NET_MATCH_NONE;
        out->kind = N64NET_MATCH_ERROR;
        std::snprintf(out->error, sizeof(out->error), "%s", f[1].c_str());
        return out->kind;
    }
    if (f.size() < 2 || f[1] != kVersion) return N64NET_MATCH_NONE;
    if (w == "PRESOK" && f.size() >= 3) {
        out->kind = N64NET_MATCH_PRESOK;
        out->online = atoi(f[2].c_str());
    } else if (w == "SEEKING" && f.size() >= 4) {
        out->kind = N64NET_MATCH_SEEKING;
        out->secs_waited = atoi(f[2].c_str());
        out->others_seeking = atoi(f[3].c_str());
    } else if (w == "MATCH" && f.size() >= 8) {
        out->kind = N64NET_MATCH_FOUND;
        std::snprintf(out->code, sizeof(out->code), "%s", f[2].c_str());
        out->is_host = f[3] == "host";
        std::snprintf(out->game, sizeof(out->game), "%s", f[4].c_str());
        std::snprintf(out->peer_device, sizeof(out->peer_device), "%s", f[5].c_str());
        Decode(f[6], out->peer_name, sizeof(out->peer_name));
        out->relay = atoi(f[7].c_str());
    } else if ((w == "PEOPLE" || w == "ROOMS") && f.size() >= 4) {
        const bool people = w == "PEOPLE";
        out->kind = people ? N64NET_MATCH_PEOPLE : N64NET_MATCH_ROOMS;
        out->page = atoi(f[2].c_str());
        out->pages = atoi(f[3].c_str());
        for (size_t i = 4; i < f.size() && out->count < N64NET_MATCH_LIST_MAX; i++) {
            const std::vector<std::string> e = Split(f[i], ',');
            if (people && e.size() >= 3) {
                N64NetMatchPerson &p = out->people[out->count++];
                Decode(e[0], p.name, sizeof(p.name));
                std::snprintf(p.state, sizeof(p.state), "%s", e[1].c_str());
                std::snprintf(p.info, sizeof(p.info), "%s", e[2].c_str());
                if (e.size() >= 4) std::snprintf(p.device, sizeof(p.device), "%s", e[3].c_str());
            } else if (!people && e.size() >= 5) {
                N64NetMatchRoom &r = out->rooms[out->count++];
                std::snprintf(r.code, sizeof(r.code), "%s", e[0].c_str());
                Decode(e[1], r.host_name, sizeof(r.host_name));
                std::snprintf(r.info, sizeof(r.info), "%s", e[2].c_str());
                r.members = atoi(e[3].c_str());
                r.max_members = atoi(e[4].c_str());
            }
        }
    } else if (w == "CALLED" && f.size() >= 5) {
        out->kind = N64NET_MATCH_CALLED;
        std::snprintf(out->peer_device, sizeof(out->peer_device), "%s", f[2].c_str());
        Decode(f[3], out->peer_name, sizeof(out->peer_name));
        std::snprintf(out->game, sizeof(out->game), "%s", f[4].c_str());
    } else if (w == "CALLING" && f.size() >= 4) {
        out->kind = N64NET_MATCH_CALLING;
        Decode(f[2], out->peer_name, sizeof(out->peer_name));
        std::snprintf(out->call_state, sizeof(out->call_state), "%s", f[3].c_str());
    } else if (w == "CALLDONE" && f.size() >= 3) {
        out->kind = N64NET_MATCH_CALLDONE;
        std::snprintf(out->peer_device, sizeof(out->peer_device), "%s", f[2].c_str());
    } else if (w == "OPENOK" && f.size() >= 4) {
        out->kind = N64NET_MATCH_OPENOK;
        std::snprintf(out->code, sizeof(out->code), "%s", f[2].c_str());
        out->open = f[3] == "1";
    } else {
        return N64NET_MATCH_NONE;
    }
    return out->kind;
}
