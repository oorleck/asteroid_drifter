// net_session.h -- the state of one end of a network match. Private to net_game.cpp, but the
// two-game test in main.cpp reads it to check what each side has been told.
#pragma once
#include "game.h"
#include <memory>

using net::Endpoint;
using net::HostReplicator;
using net::ClientReplicator;
using net::Link;
using net::UdpLink;

struct NetSession {
    bool host = false;
    Link* link = nullptr;
    std::unique_ptr<UdpLink> udp;                   // when the game made the socket itself
    double now = 0;
    int frame = 0;
    std::string status;

    // ---- host: one Client per connection, indexed by the link's peer number
    struct Client {
        Endpoint ep;
        HostReplicator sync;                         // what this client believes about the rocks
        bool used = false, joined = false;
        int playerId = -1;
        double lastHeard = 0;
        uint16_t lastCmd = 0;
        bool haveCmd = false;
    };
    Client clients[net::MAX_PLAYERS];
    HostReplicator master;                           // drains the journal, once, for everybody

    // ---- client
    Endpoint ep;
    ClientReplicator crep;
    int myId = -1;
    bool welcomed = false, lost = false, rejected = false;
    double lastHeard = 0, helloAt = -1, startedAt = 0;
    uint16_t cmdSeq = 0;
    uint64_t bytesAtMark = 0;  double markAt = 0;  double kbps = 0;
    int lastHealth = 100;
};
