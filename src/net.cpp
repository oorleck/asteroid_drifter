// net.cpp -- the transport: the loopback link, Winsock UDP, and the reliable
// ordered channel that sits on top of either of them.
#include "net.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif

namespace net {

// ------------------------------------------------------------- LoopLink --
bool LoopLink::send(const uint8_t* d, size_t n, int peer) {
    if (!other || n == 0 || n > MAX_PACKET) return false;
    bytesSent += n;
    ++packetsSent;
    // Drop a share of packets on the floor if asked, so the reliable channel has
    // something to prove itself against.
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    if (loss > 0.0f && (float)(rng >> 8) * (1.0f / 16777216.0f) < loss) return true;
    Pending p;
    p.data.assign(d, d + n);
    p.peer = myPeer;                       // the far end sees who it came from
    p.due = other->now + latency;
    other->queue.push_back(std::move(p));
    return true;
}

bool LoopLink::recv(Packet& out) {
    for (size_t i = 0; i < queue.size(); ++i) {
        if (queue[i].due > now) continue;
        out.data = std::move(queue[i].data);
        out.peer = queue[i].peer;
        queue.erase(queue.begin() + (std::ptrdiff_t)i);
        bytesRecv += out.data.size();
        ++packetsRecv;
        return true;
    }
    return false;
}

// -------------------------------------------------------------- UdpLink --
namespace {
bool wsaReady = false;
bool startWsa() {
    if (wsaReady) return true;
    WSADATA d;
    if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return false;
    wsaReady = true;
    return true;
}
}  // namespace

bool UdpLink::open(int port) {
    if (!startWsa()) { lastError = "WSAStartup failed"; return false; }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { lastError = "socket() failed"; return false; }
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons((u_short)port);
    if (bind(s, (sockaddr*)&a, sizeof a) == SOCKET_ERROR) {
        char buf[96];
        snprintf(buf, sizeof buf, "could not bind port %d (error %d)", port, WSAGetLastError());
        lastError = buf;
        closesocket(s);
        return false;
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    sock = (uintptr_t)s;
    host = true;
    return true;
}

bool UdpLink::connect(const char* ip, int port) {
    if (!startWsa()) { lastError = "WSAStartup failed"; return false; }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { lastError = "socket() failed"; return false; }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    sock = (uintptr_t)s;
    host = false;
    in_addr addr = {};
    if (inet_pton(AF_INET, ip, &addr) != 1) { lastError = std::string("bad address: ") + ip; return false; }
    peers[0].ip = addr.s_addr;
    peers[0].port = htons((u_short)port);
    peers[0].used = true;
    peerCount = 1;
    return true;
}

int UdpLink::findOrAddPeer(uint32_t ip, uint16_t port) {
    for (int i = 0; i < MAX_PLAYERS; ++i)
        if (peers[i].used && peers[i].ip == ip && peers[i].port == port) return i;
    for (int i = 0; i < MAX_PLAYERS; ++i)
        if (!peers[i].used) {
            peers[i].ip = ip;  peers[i].port = port;  peers[i].used = true;
            ++peerCount;
            return i;
        }
    return -1;
}

bool UdpLink::send(const uint8_t* d, size_t n, int peer) {
    if (sock == ~(uintptr_t)0 || n == 0 || n > MAX_PACKET) return false;
    if (peer < 0 || peer >= MAX_PLAYERS || !peers[peer].used) return false;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = peers[peer].ip;
    a.sin_port = peers[peer].port;
    const int sent = sendto((SOCKET)sock, (const char*)d, (int)n, 0, (sockaddr*)&a, sizeof a);
    if (sent <= 0) return false;
    bytesSent += (uint64_t)sent;
    ++packetsSent;
    return true;
}

bool UdpLink::recv(Packet& out) {
    if (sock == ~(uintptr_t)0) return false;
    uint8_t buf[MAX_PACKET + 64];
    sockaddr_in from = {};
    int flen = sizeof from;
    const int n = recvfrom((SOCKET)sock, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &flen);
    if (n <= 0) return false;
    const int peer = findOrAddPeer(from.sin_addr.s_addr, from.sin_port);
    if (peer < 0) return false;
    out.data.assign(buf, buf + n);
    out.peer = peer;
    bytesRecv += (uint64_t)n;
    ++packetsRecv;
    return true;
}

void UdpLink::close() {
    if (sock != ~(uintptr_t)0) { closesocket((SOCKET)sock); sock = ~(uintptr_t)0; }
}

// ------------------------------------------------------------- Reliable --
void Reliable::queue(const std::vector<uint8_t>& payload) {
    Out o;
    o.seq = nextSeq++;
    o.payload = payload;
    o.sentAt = -1e9;               // never sent, so the next collect() picks it up
    o.tries = 0;
    outbox.push_back(std::move(o));
}

void Reliable::collect(double now, std::vector<std::vector<uint8_t>>& out) {
    for (Out& o : outbox) {
        if (now - o.sentAt < resendAfter) continue;
        if (o.tries >= maxTries) continue;
        o.sentAt = now;
        if (o.tries > 0) ++resent;
        ++o.tries;
        std::vector<uint8_t> framed;
        framed.reserve(o.payload.size() + 4);
        for (int i = 0; i < 4; ++i) framed.push_back((uint8_t)(o.seq >> (8 * i)));
        framed.insert(framed.end(), o.payload.begin(), o.payload.end());
        out.push_back(std::move(framed));
    }
}

void Reliable::rttSample(double s) {
    if (s < 0.0 || s > 5.0) return;
    srtt = srtt == 0.0 ? s : srtt * 0.875 + s * 0.125;
    // Wait a little over one round trip before assuming a message was lost.
    resendAfter = std::max(0.12, std::min(1.5, srtt * 1.5 + 0.06));
}

void Reliable::ack(uint32_t through) {
    if (through > ackedThrough) ackedThrough = through;
    outbox.erase(std::remove_if(outbox.begin(), outbox.end(),
                                [&](const Out& o) { return o.seq <= ackedThrough; }),
                 outbox.end());
}

void Reliable::accept(uint32_t seq, const uint8_t* d, size_t n,
                      std::vector<std::vector<uint8_t>>& ready) {
    if (seq < wantSeq) return;                       // a repeat of something already handled
    if (seq == wantSeq) {
        ready.emplace_back(d, d + n);
        ++wantSeq;
        // Anything that arrived early and was held now has its turn.
        bool moved = true;
        while (moved) {
            moved = false;
            for (size_t i = 0; i < holding.size(); ++i) {
                if (holding[i].seq != wantSeq) continue;
                ready.push_back(std::move(holding[i].payload));
                holding.erase(holding.begin() + (std::ptrdiff_t)i);
                ++wantSeq;
                moved = true;
                break;
            }
        }
        return;
    }
    for (const In& h : holding) if (h.seq == seq) return;      // already holding it
    In h;
    h.seq = seq;
    h.payload.assign(d, d + n);
    holding.push_back(std::move(h));
}

}  // namespace net

// -------------------------------------------------------------- Endpoint --
namespace net {

namespace {
constexpr size_t FRAGMENT = 900;             // payload bytes per fragment: well inside one packet
constexpr uint8_t FRAG_MORE = 0xFE, FRAG_LAST = 0xFF;   // message ids are all below 0x80
constexpr size_t HEADER = 11;                // kind, ack, stamp, echo, hold
}

void Endpoint::sendReliable(const std::vector<uint8_t>& payload) {
    if (payload.size() <= FRAGMENT) { rel.queue(payload); return; }
    for (size_t o = 0; o < payload.size(); o += FRAGMENT) {
        const size_t n = std::min(FRAGMENT, payload.size() - o);
        std::vector<uint8_t> f;
        f.reserve(n + 1);
        f.push_back(o + n >= payload.size() ? FRAG_LAST : FRAG_MORE);
        f.insert(f.end(), payload.begin() + (std::ptrdiff_t)o, payload.begin() + (std::ptrdiff_t)(o + n));
        rel.queue(f);
    }
}

// Every packet begins with the same header.
static void writeHeader(std::vector<uint8_t>& pk, uint8_t kind, uint32_t ack, double now,
                        bool haveStamp, uint16_t lastStamp, double lastStampAt) {
    pk.push_back(kind);
    for (int i = 0; i < 4; ++i) pk.push_back((uint8_t)(ack >> (8 * i)));
    const uint16_t stamp = (uint16_t)((uint32_t)(now * 1000.0) & 0xFFFF);
    pk.push_back((uint8_t)stamp);  pk.push_back((uint8_t)(stamp >> 8));
    const uint16_t echo = haveStamp ? lastStamp : 0;
    const uint16_t hold = haveStamp ? (uint16_t)std::min(65534.0, std::max(0.0, (now - lastStampAt) * 1000.0)) : 0xFFFF;
    pk.push_back((uint8_t)echo);   pk.push_back((uint8_t)(echo >> 8));
    pk.push_back((uint8_t)hold);   pk.push_back((uint8_t)(hold >> 8));
}

void Endpoint::sendUnreliable(const std::vector<uint8_t>& payload) {
    if (!link || payload.empty() || payload.size() + HEADER > (size_t)MAX_PACKET) return;
    std::vector<uint8_t> pk;
    pk.reserve(payload.size() + HEADER);
    writeHeader(pk, 2, rel.wantSeq - 1, now, haveStamp, lastStamp, lastStampAt);
    pk.insert(pk.end(), payload.begin(), payload.end());
    link->send(pk.data(), pk.size(), peer);
    ++unreliableSent;
    wantsAck = false;               // the ack rode along
}

void Endpoint::flush(double t) {
    now = t;
    if (!link) return;
    std::vector<std::vector<uint8_t>> framed;         // each is seq:u32 + payload
    rel.collect(now, framed);

    const uint32_t ack = rel.wantSeq - 1;
    std::vector<uint8_t> pk;
    bool open = false;
    for (const auto& m : framed) {
        const size_t need = 2 + m.size();
        if (open && pk.size() + need > (size_t)MAX_PACKET) {
            link->send(pk.data(), pk.size(), peer);
            ++reliableSent;
            open = false;
        }
        if (!open) { pk.clear(); writeHeader(pk, 1, ack, now, haveStamp, lastStamp, lastStampAt); open = true; }
        pk.push_back((uint8_t)(m.size() & 0xFF));
        pk.push_back((uint8_t)(m.size() >> 8));
        pk.insert(pk.end(), m.begin(), m.end());
    }
    if (open) {
        link->send(pk.data(), pk.size(), peer);
        ++reliableSent;
        wantsAck = false;
    }
    if (wantsAck) {                                   // nothing else to carry it
        pk.clear();
        writeHeader(pk, 0, ack, now, haveStamp, lastStamp, lastStampAt);
        link->send(pk.data(), pk.size(), peer);
        ++ackOnlySent;
        wantsAck = false;
    }
}

void Endpoint::poll(double t) {
    now = t;
    if (!link) return;
    Packet p;
    std::vector<std::vector<uint8_t>> got;
    while (link->recv(p)) {
        Reader r(p.data.data(), p.data.size());
        const uint8_t kind = r.u8();
        const uint32_t ack = r.u32();
        const uint16_t stamp = r.u16(), echo = r.u16(), hold = r.u16();
        if (r.bad) continue;
        haveStamp = true;  lastStamp = stamp;  lastStampAt = now;
        if (hold != 0xFFFF) {
            // Our clock now, minus the stamp of ours they echoed, minus how long they held it.
            const uint16_t nowMs = (uint16_t)((uint32_t)(now * 1000.0) & 0xFFFF);
            const uint16_t ms = (uint16_t)(nowMs - echo - hold);
            if (ms < 5000) rel.rttSample(ms / 1000.0);
        }
        rel.ack(ack);
        if (kind == 1) {
            while (r.left() >= 6) {
                const uint16_t len = r.u16();
                const uint32_t seq = r.u32();
                if (r.bad || len < 4 || (size_t)(len - 4) > r.left()) break;
                const size_t payload = (size_t)len - 4;
                rel.accept(seq, r.p + r.at, payload, got);
                r.at += payload;
            }
            wantsAck = true;
        } else if (kind == 2) {
            unreliable.emplace_back(r.p + r.at, r.p + r.n);
        }
    }
    // Put large messages back together. They arrive in order, so this is a matter
    // of gluing fragments until the last one.
    for (auto& m : got) {
        if (m.empty()) continue;
        if (m[0] == FRAG_MORE || m[0] == FRAG_LAST) {
            assembling.insert(assembling.end(), m.begin() + 1, m.end());
            if (m[0] == FRAG_LAST) { ready.push_back(std::move(assembling)); assembling.clear(); }
        } else {
            ready.push_back(std::move(m));
        }
    }
}

}  // namespace net
