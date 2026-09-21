// net_game.cpp -- versus over a network: one machine hosts, the others join.
//
// The host runs the only real simulation, exactly as in an offline match: it moves
// every player (a client's player is moved by the commands that client sends), fires
// their rifles, decides who was hit and who died, and carves the rocks. It then tells
// each client three things:
//
//   * what happened to the world's shape (net.h: the journal), reliably;
//   * where the rocks that have moved are, and where every player is, unreliably
//     and often, since the next one replaces it;
//   * events that will not be repeated: a round was fired, someone was killed.
//
// A client sends only its commands. It draws what the host says, but it does not
// wait for the host to move its own spaceman: it runs the same stepPlayer() locally
// against the rocks it has been told about (predicting), and eases toward the
// host's version of it whenever a snapshot arrives. Other players are only ever
// told where they are, so they glide between snapshots.
#include "game.h"
#include "net.h"
#include "net_session.h"
#include <algorithm>
#include <cstdio>
#include <memory>

using namespace net;

namespace {
const Col TINTS[8] = {
    Col(0.85f, 0.95f, 1.00f), Col(1.00f, 0.45f, 0.35f), Col(0.40f, 1.00f, 0.55f), Col(1.00f, 0.85f, 0.30f),
    Col(0.75f, 0.50f, 1.00f), Col(0.30f, 0.85f, 1.00f), Col(1.00f, 0.55f, 0.85f), Col(0.85f, 0.70f, 0.45f),
};
constexpr double TIMEOUT = 6.0;                  // silence this long and a peer is gone
constexpr uint8_t NOBODY = 255;

void putName(Writer& w, const char* name) {
    const size_t n = std::min<size_t>(strlen(name), 15);
    w.u8((uint8_t)n);
    for (size_t i = 0; i < n; ++i) w.u8((uint8_t)name[i]);
}
void getName(Reader& r, char* out) {
    const size_t n = std::min<size_t>(r.u8(), 15);
    for (size_t i = 0; i < n; ++i) out[i] = (char)r.u8();
    out[n] = 0;
}
// Is a newer than b, allowing for the counter wrapping round?
bool newer(uint16_t a, uint16_t b) { return (uint16_t)(a - b) < 32768 && a != b; }
}  // namespace


// ------------------------------------------------------------------ helpers --
Bullet Game::makeBullet(const Player& p, dv2 pos, v2 vel, bool heavy) const {
    Bullet b;
    b.pos = pos;
    b.vel = vel;
    b.caliber = heavy ? tune::HEAVY_CAL : tune::BULLET_CAL;
    b.gravScale = heavy ? tune::HEAVY_GRAV : tune::BULLET_GRAV;
    b.budget = heavy ? tune::HEAVY_PEN : tune::BULLET_PEN;
    b.life = heavy ? rules::HOMING_LIFE : 2.6f;
    b.heavy = heavy;
    b.owner = p.id;
    b.col = mix(heavy ? Col(1.00f, 0.45f, 0.75f) : Col(1.00f, 0.86f, 0.45f), p.tint, 0.55f);
    return b;
}

bool Game::netConnected() const { return net && (net->host || (net->welcomed && !net->lost)); }
float Game::netRtt() const { return net && !net->host ? (float)net->ep.rel.srtt : 0.0f; }

std::string Game::netStatus() const {
    if (!net) return "";
    char buf[160];
    if (net->host) {
        int n = 0;
        for (const auto& c : net->clients) if (c.joined) ++n;
        snprintf(buf, sizeof buf, "HOSTING   %d PLAYER%s CONNECTED", n, n == 1 ? "" : "S");
        return buf;
    }
    if (net->rejected) return "THE HOST REFUSED THE CONNECTION";
    if (net->lost)     return "CONNECTION LOST";
    if (!net->welcomed) return net->now - net->startedAt > 4.0 ? "COULD NOT REACH THE HOST" : "CONNECTING...";
    snprintf(buf, sizeof buf, "CONNECTED   PING %.0f MS   %.0f KB/S", net->ep.rel.srtt * 1000.0, net->kbps);
    return buf;
}

void Game::netShutdown() {
    if (!net) return;
    if (net->host) {
        Writer w;  w.u8((uint8_t)Msg::Bye);
        for (auto& c : net->clients) if (c.joined) { c.ep.sendReliable(w.b); c.ep.flush(net->now); }
    } else if (net->welcomed && !net->lost) {
        Writer w;  w.u8((uint8_t)Msg::Bye);
        net->ep.sendReliable(w.b);
        for (int i = 0; i < 3; ++i) net->ep.sendUnreliable(w.b);      // a goodbye is worth saying a few times
        net->ep.flush(net->now);
    }
    delete net;
    net = nullptr;
    netHost = false;
    netClient = false;
}

// ------------------------------------------------------------------- startup --
bool Game::startHostOn(Renderer& r, Link* link, int bots) {
    if (net) netShutdown();
    startVersus(r, bots);
    net = new NetSession();
    net->host = true;
    net->link = link;
    netHost = true;
    snprintf(pl.name, sizeof pl.name, "%s", strcmp(localName, "PILOT") ? localName : "HOST");
    netClient = false;
    net->master.begin(world);                       // from here on, every change to the world is journalled
    for (auto& c : net->clients) c.sync.attach(world);
    return true;
}

bool Game::startHost(Renderer& r, int port, int bots, std::string* err, bool loopbackOnly) {
    auto udp = std::make_unique<UdpLink>();
    if (!udp->open(port, loopbackOnly)) {
        if (err) *err = udp->lastError;
        return false;
    }
    Link* raw = udp.get();
    if (!startHostOn(r, raw, bots)) return false;
    net->udp = std::move(udp);
    return true;
}

bool Game::startClientOn(Renderer& r, Link* link) {
    if (net) netShutdown();
    // A client's world starts empty and is filled by the host.
    ++runCount;
    r.arenaReset();
    world.init(0);
    bullets.clear();  parts.clear();  waves.clear();
    enemies.clear();  ebullets.clear();  missiles.clear();
    nukes.clear();  pmissiles.clear();  ships.clear();
    peers.clear();  killFeed.clear();
    sandbox = true;  versus = true;
    state = State::Playing;
    stateTime = 0;
    match = Match();
    pl = Player();
    pl.hasHoming = true;
    pl.dead = true;                                 // until the host says where to put us
    pl.tint = TINTS[0];
    snprintf(pl.name, sizeof pl.name, "%s", localName);
    netClient = true;
    netHost = false;

    net = new NetSession();
    net->host = false;
    net->link = link;
    net->ep.link = link;
    net->ep.peer = 0;
    net->crep.begin(world);
    net->startedAt = 0;
    Writer w;
    w.u8((uint8_t)Msg::Hello);
    w.u32(PROTOCOL);
    putName(w, pl.name);
    net->ep.sendReliable(w.b);
    net->helloAt = 0;
    return true;
}

bool Game::startClient(Renderer& r, const char* ip, int port, std::string* err) {
    auto udp = std::make_unique<UdpLink>();
    if (!udp->connect(ip, port)) {
        if (err) *err = udp->lastError;
        return false;
    }
    Link* raw = udp.get();
    if (!startClientOn(r, raw)) return false;
    net->udp = std::move(udp);
    return true;
}

// ------------------------------------------------------------ host: reading --
static void hostSendInfo(Game& g, NetSession::Client& c, const Player& p) {
    Writer w;
    w.u8((uint8_t)Msg::PlayerInfo);
    w.u8((uint8_t)p.id);
    putName(w, p.name);
    w.f32(p.tint.r);  w.f32(p.tint.g);  w.f32(p.tint.b);
    c.ep.sendReliable(w.b);
    (void)g;
}

static void hostAccept(Game& g, NetSession& n, int idx, Reader& r) {
    NetSession::Client& c = n.clients[idx];
    if (c.joined) return;                            // a repeated Hello
    const uint32_t proto = r.u32();
    char name[16] = {};
    getName(r, name);
    if (r.bad || proto != PROTOCOL || (int)g.peers.size() >= MAX_PLAYERS - 1) {
        Writer w;  w.u8((uint8_t)Msg::Reject);  w.u8(proto != PROTOCOL ? 1 : 2);
        c.ep.sendReliable(w.b);
        return;
    }
    // The lowest id not in use.
    int id = 1;
    while (g.playerById(id)) ++id;

    Game::Peer p;
    p.bot = false;
    p.netPeer = idx;
    p.body.id = id;
    if (name[0] && strcmp(name, "PILOT")) snprintf(p.body.name, sizeof p.body.name, "%s", name);
    else                                    snprintf(p.body.name, sizeof p.body.name, "PILOT %d", id);
    p.body.tint = TINTS[id % 8];
    p.body.hasHoming = true;
    g.peers.push_back(p);
    g.respawnPlayer(g.peers.back().body);
    c.playerId = id;
    c.joined = true;

    // Tell it who it is, who else is here, and what the world looks like.
    Writer w;
    w.u8((uint8_t)Msg::Welcome);
    w.u8((uint8_t)id);
    w.u8((uint8_t)g.match.round);
    c.ep.sendReliable(w.b);
    hostSendInfo(g, c, g.pl);
    for (const Game::Peer& o : g.peers) hostSendInfo(g, c, o.body);
    std::vector<std::vector<uint8_t>> rocks;
    c.sync.attach(g.world);
    c.sync.collectInitial(rocks);
    for (auto& m : rocks) c.ep.sendReliable(m);

    // ...and tell everybody else it has arrived.
    for (int i = 0; i < MAX_PLAYERS; ++i)
        if (i != idx && n.clients[i].joined) hostSendInfo(g, n.clients[i], g.peers.back().body);
}

static void hostDrop(Game& g, NetSession& n, int idx) {
    NetSession::Client& c = n.clients[idx];
    if (!c.joined) return;
    const int id = c.playerId;
    g.peers.erase(std::remove_if(g.peers.begin(), g.peers.end(),
                                 [&](const Game::Peer& p) { return p.netPeer == idx && !p.bot; }), g.peers.end());
    c.joined = false;  c.used = false;  c.playerId = -1;  c.haveCmd = false;
    c.ep = Endpoint();
    c.sync = HostReplicator();
    c.sync.attach(g.world);
    Writer w;  w.u8((uint8_t)Msg::PlayerLeft);  w.u8((uint8_t)id);
    for (auto& o : n.clients) if (o.joined) o.ep.sendReliable(w.b);
}

static void hostRead(Game& g, NetSession& n) {
    Packet p;
    while (n.link->recv(p)) {
        if (p.peer < 0 || p.peer >= MAX_PLAYERS) continue;
        NetSession::Client& c = n.clients[p.peer];
        if (!c.used) {
            c.used = true;
            c.ep.link = n.link;
            c.ep.peer = p.peer;
            c.sync.attach(g.world);
        }
        c.lastHeard = n.now;
        c.ep.feed(p, n.now);
    }
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        NetSession::Client& c = n.clients[i];
        if (!c.used) continue;
        std::vector<std::vector<uint8_t>> msgs;
        msgs.swap(c.ep.ready);
        for (auto& m : msgs) {
            Reader r(m.data(), m.size());
            const Msg kind = (Msg)r.u8();
            if (kind == Msg::Hello)        hostAccept(g, n, i, r);
            else if (kind == Msg::RockRepair && c.joined) {
                const uint16_t k = r.u16();
                for (uint16_t j = 0; j < k && !r.bad; ++j) {
                    std::vector<uint8_t> f;
                    c.sync.writeField(r.u32(), f);
                    if (!f.empty()) c.ep.sendReliable(f);
                }
            } else if (kind == Msg::Bye)   hostDrop(g, n, i);
        }
        // Commands: the newest wins.
        for (auto& m : c.ep.unreliable) {
            Reader r(m.data(), m.size());
            const Msg um = (Msg)r.u8();
            if (um == Msg::Bye) { hostDrop(g, n, i); break; }
            if (um != Msg::Input || !c.joined) continue;
            const uint16_t seq = r.u16();
            PlayerCmd cmd;
            cmd.aim = r.f32();
            cmd.move = r.f32();
            const uint8_t flags = r.u8();
            cmd.thrust = flags & 1;
            cmd.fire = flags & 2;
            cmd.jumpSeq = r.u8();
            cmd.heavySeq = r.u8();
            if (r.bad) continue;
            if (c.haveCmd && !newer(seq, c.lastCmd)) continue;      // late or repeated
            c.haveCmd = true;
            c.lastCmd = seq;
            for (Game::Peer& pe : g.peers)
                if (pe.netPeer == i && !pe.bot) pe.cmd = cmd;
        }
        c.ep.unreliable.clear();
        if (c.joined && n.now - c.lastHeard > TIMEOUT) hostDrop(g, n, i);
    }
}

// ---------------------------------------------------------- host: publishing --
static void writePlayer(Writer& w, const Player& p) {
    w.u8((uint8_t)p.id);
    uint8_t flags = 0;
    if (p.dead) flags |= 1;
    if (p.thrusting) flags |= 2;
    if (p.grounded) flags |= 4;
    if (p.protect > 0.0f) flags |= 8;
    w.u8(flags);
    w.dvec(p.pos);
    w.vec(p.vel);
    w.f32(std::atan2(p.up.y, p.up.x));
    w.f32(p.aim);
    w.u8((uint8_t)clampi((int)std::lround(p.health), 0, 100));
    w.u8((uint8_t)clampi((int)std::lround(p.fuel), 0, 100));
    w.u16((uint16_t)(int16_t)p.frags);
    w.u16((uint16_t)(int16_t)p.deaths);
    w.f32(p.respawnIn);
}

static void hostPublish(Game& g, NetSession& n) {
    // The world's changes, once, to everyone who has joined.
    std::vector<std::vector<uint8_t>> shape;
    n.master.collectShapeChanges(shape);
    n.master.collectFullSyncs(shape);
    for (auto& c : n.clients)
        if (c.joined) for (auto& m : shape) c.ep.sendReliable(m);

    // Where everyone is: 20 times a second.
    std::vector<uint8_t> snap;
    if (n.frame % 3 == 0) {
        Writer w;
        w.u8((uint8_t)Msg::PlayerState);
        w.u16(0);
        w.u8((uint8_t)g.match.round);
        w.u8(g.match.over ? (uint8_t)(1 | ((g.match.winner & 0x7F) << 1)) : 0);
        w.f32(g.match.overTime);
        const size_t countAt = w.b.size();
        w.u8(0);
        uint8_t k = 0;
        writePlayer(w, g.pl);  ++k;
        for (const Game::Peer& pe : g.peers) { writePlayer(w, pe.body); ++k; }
        w.b[countAt] = k;
        snap = std::move(w.b);
    }

    for (int i = 0; i < MAX_PLAYERS; ++i) {
        NetSession::Client& c = n.clients[i];
        if (!c.used) continue;
        if (c.joined) {
            if (n.frame % 2 == 0) {
                std::vector<uint8_t> mo;
                c.sync.collectMotion(n.now, mo);
                if (!mo.empty()) c.ep.sendUnreliable(mo);
                std::vector<uint8_t> au;
                c.sync.collectAudit(n.now, au);
                if (!au.empty()) c.ep.sendReliable(au);
            }
            if (!snap.empty()) c.ep.sendUnreliable(snap);
        }
        c.ep.flush(n.now);
    }
}

void Game::netShot(const Bullet& b) {
    if (!net || !net->host) return;
    Writer w;
    w.u8((uint8_t)Msg::Shot);
    w.u8((uint8_t)b.owner);
    w.dvec(b.pos);
    w.vec(b.vel);
    w.u8(b.heavy ? 1 : 0);
    for (auto& c : net->clients) {
        if (!c.joined || c.playerId == b.owner) continue;       // a client already drew its own
        c.ep.sendReliable(w.b);
    }
}

void Game::netKilled(int killer, int victim, dv2 at) {
    if (!net || !net->host) return;
    Writer w;
    w.u8((uint8_t)Msg::Kill);
    w.u8(killer < 0 ? NOBODY : (uint8_t)killer);
    w.u8((uint8_t)victim);
    w.dvec(at);
    for (auto& c : net->clients) if (c.joined) c.ep.sendReliable(w.b);
}

// ---------------------------------------------------------- client: reading --
static Game::Peer* peerFor(Game& g, int id) {
    for (Game::Peer& p : g.peers) if (p.body.id == id) return &p;
    Game::Peer p;
    p.bot = false;
    p.body.id = id;
    snprintf(p.body.name, sizeof p.body.name, "PILOT %d", id);
    p.body.tint = TINTS[id % 8];
    p.body.hasHoming = true;
    p.body.dead = true;
    g.peers.push_back(p);
    return &g.peers.back();
}

static void clientSnapshot(Game& g, NetSession& n, Reader& r) {
    if (!n.welcomed) return;                           // we do not yet know which of these players is us
    r.u16();
    const int round = r.u8();
    const uint8_t over = r.u8();
    const float overTime = r.f32();
    const int count = r.u8();
    if (r.bad) return;

    if (round != g.match.round) {                    // a new match: forget the old one's clutter
        g.match.round = round;
        g.killFeed.clear();
        g.bullets.clear();
    }
    g.match.over = over & 1;
    g.match.winner = g.match.over ? (over >> 1) : -1;
    g.match.overTime = overTime;

    for (int i = 0; i < count; ++i) {
        const int id = r.u8();
        const uint8_t flags = r.u8();
        const dv2 pos = r.dvec();
        const v2 vel = r.vec();
        const float upAng = r.f32(), aim = r.f32();
        const int health = r.u8(), fuel = r.u8();
        const int frags = (int16_t)r.u16(), deaths = (int16_t)r.u16();
        const float respawnIn = r.f32();
        if (r.bad) return;

        const bool dead = flags & 1;
        if (id == n.myId) {
            Player& p = g.pl;
            const bool wasDead = p.dead;
            if (health < n.lastHealth && !dead) {
                g.sfxUI(Sfx::Hurt, clampf((n.lastHealth - health) / 22.0f, 0.35f, 1.0f));
                g.shake = std::max(g.shake, 0.4f);
                p.hurtGlow = std::min(1.0f, p.hurtGlow + (n.lastHealth - health) * 0.04f);
            }
            n.lastHealth = health;
            p.dead = dead;
            p.health = (float)health;
            p.frags = frags;  p.deaths = deaths;
            p.respawnIn = respawnIn;
            p.protect = (flags & 8) ? 1.0f : 0.0f;
            if (!dead) {
                if (wasDead) {                        // the host has put us somewhere: go there
                    p.pos = pos;  p.vel = vel;
                    p.up = fromAngle(upAng);
                    p.fuel = (float)fuel;
                    p.grounded = false;  p.ground = BodyRef();
                    p.fireCd = p.heavyCd = p.jumpCd = 0.0f;
                    g.cam.pos = p.pos;
                } else {
                    // Ease toward where the host says we are, allowing for the time the news took.
                    const float lead = (float)n.ep.rel.srtt;
                    const dv2 want(pos.x + vel.x * lead, pos.y + vel.y * lead);
                    const double dx = want.x - p.pos.x, dy = want.y - p.pos.y;
                    if (dx * dx + dy * dy > 300.0 * 300.0) { p.pos = want; p.vel = vel; }
                    else {
                        p.pos.x += dx * 0.18;  p.pos.y += dy * 0.18;
                        p.vel += (vel - p.vel) * 0.18f;
                    }
                }
            }
        } else {
            Game::Peer* pe = peerFor(g, id);
            Player& b = pe->body;
            const bool wasDead = b.dead;
            b.dead = dead;
            b.health = (float)health;
            b.fuel = (float)fuel;
            b.frags = frags;  b.deaths = deaths;
            b.respawnIn = respawnIn;
            b.protect = (flags & 8) ? 1.0f : 0.0f;
            b.thrusting = flags & 2;
            b.grounded = flags & 4;
            b.aim = aim;
            b.up = fromAngle(upAng);
            pe->netPos = pos;  pe->netVel = vel;  pe->netAt = n.now;
            if (!pe->netHave || wasDead != dead || dead) { b.pos = pos;  b.vel = vel; }
            pe->netHave = true;
        }
    }
}

static void clientRead(Game& g, NetSession& n) {
    n.ep.poll(n.now);
    if (!n.ep.ready.empty() || !n.ep.unreliable.empty()) n.lastHeard = n.now;

    std::vector<std::vector<uint8_t>> msgs;
    msgs.swap(n.ep.ready);
    for (auto& m : msgs) {
        if (m.empty()) continue;
        Reader r(m.data(), m.size());
        const Msg kind = (Msg)m[0];
        switch (kind) {
        case Msg::Welcome: {
            r.u8();
            n.myId = r.u8();
            g.match.round = r.u8();
            g.pl.id = n.myId;
            g.pl.tint = TINTS[n.myId % 8];
            g.peers.erase(std::remove_if(g.peers.begin(), g.peers.end(),         // never an opponent to ourselves
                                         [&](const Game::Peer& p) { return p.body.id == n.myId; }), g.peers.end());
            n.welcomed = true;
            n.lastHeard = n.now;
        } break;
        case Msg::Reject:  n.rejected = true; break;
        case Msg::PlayerInfo: {
            r.u8();
            const int id = r.u8();
            char name[16] = {};
            getName(r, name);
            const Col c(r.f32(), r.f32(), r.f32());
            if (r.bad) break;
            if (id == n.myId) { snprintf(g.pl.name, sizeof g.pl.name, "%s", name); g.pl.tint = c; }
            else { Game::Peer* p = peerFor(g, id); snprintf(p->body.name, sizeof p->body.name, "%s", name); p->body.tint = c; }
        } break;
        case Msg::PlayerLeft: {
            r.u8();
            const int id = r.u8();
            g.peers.erase(std::remove_if(g.peers.begin(), g.peers.end(),
                                         [&](const Game::Peer& p) { return p.body.id == id; }), g.peers.end());
        } break;
        case Msg::Shot: {
            r.u8();
            const int owner = r.u8();
            const dv2 pos = r.dvec();
            const v2 vel = r.vec();
            const bool heavy = r.u8() != 0;
            if (r.bad) break;
            const Player* who = g.playerById(owner);
            Player dummy;  dummy.id = owner;
            Bullet b = g.makeBullet(who ? *who : dummy, pos, vel, heavy);
            g.bullets.push_back(b);
            g.sfx(heavy ? Sfx::Shell : Sfx::Rifle, pos, heavy ? 0.9f : 0.6f, heavy ? 1.0f : g.sfxRng.range(0.93f, 1.08f));
        } break;
        case Msg::Kill: {
            r.u8();
            const int killer = r.u8() == NOBODY ? -1 : (int)m[1];
            const int victim = r.u8();
            const dv2 at = r.dvec();
            if (!r.bad) g.killEffects(killer, victim, at);
        } break;
        case Msg::Bye: n.lost = true; break;
        case Msg::RockRepair: break;
        default:
            n.crep.applyReliable(m.data(), m.size());
            if (kind == Msg::RockAudit && !n.crep.diverged.empty()) {
                Writer w;
                w.u8((uint8_t)Msg::RockRepair);
                w.u16((uint16_t)n.crep.diverged.size());
                for (uint32_t id : n.crep.diverged) w.u32(id);
                n.ep.sendReliable(w.b);
                n.crep.diverged.clear();
            }
            break;
        }
    }

    std::vector<std::vector<uint8_t>> lossy;
    lossy.swap(n.ep.unreliable);
    for (auto& m : lossy) {
        if (m.empty()) continue;
        Reader r(m.data(), m.size());
        const Msg kind = (Msg)r.u8();
        if (kind == Msg::RockMotion) n.crep.applyUnreliable(m.data(), m.size());
        else if (kind == Msg::PlayerState) clientSnapshot(g, n, r);
    }

    if (n.welcomed && !n.lost && n.now - n.lastHeard > TIMEOUT) n.lost = true;
}

// --------------------------------------------------------------- the frame --
void Game::netBegin(float dt) {
    if (!net) return;
    net->now += dt;
    ++net->frame;
    if (net->host) { hostRead(*this, *net); return; }

    clientRead(*this, *net);

    // The rocks are the host's business: we only move them along and index them.
    net->crep.deadReckon(dt);
    world.refreshIndex(pl.pos);

    // Everyone else glides toward where the host last said they were.
    for (Peer& pe : peers) {
        if (!pe.netHave || pe.body.dead) continue;
        Player& b = pe.body;
        b.pos.x += (double)b.vel.x * dt;
        b.pos.y += (double)b.vel.y * dt;
        const double age = net->now - pe.netAt;
        const dv2 want(pe.netPos.x + pe.netVel.x * age, pe.netPos.y + pe.netVel.y * age);
        const float k = 1.0f - std::exp(-12.0f * dt);
        b.pos.x += (want.x - b.pos.x) * k;
        b.pos.y += (want.y - b.pos.y) * k;
        b.vel += (pe.netVel - b.vel) * k;
        b.facing = dot(fromAngle(b.aim), v2(b.up.y, -b.up.x)) >= 0 ? 1.0f : -1.0f;
        b.legPhase += len(b.vel) * dt * 0.02f;
        b.thrustGlow = b.thrusting ? 1.0f : approach(b.thrustGlow, 0.0f, 9.0f, dt);
        b.hurtGlow = approach(b.hurtGlow, 0.0f, 3.0f, dt);
        if (b.thrusting && rng.f() < dt * 90.0f) {
            const v2 ad = fromAngle(b.aim);
            spawnSparks(dv2(b.pos.x - ad.x * 10, b.pos.y - ad.y * 10), b.vel - ad * 260.0f, 2, 90.0f, Col(1.0f, 0.55f, 0.15f), 0.30f);
        }
    }
}

void Game::netEnd(float dt) {
    if (!net) return;
    (void)dt;
    if (net->host) { hostPublish(*this, *net); return; }

    // Our commands, every frame; the newest one is all the host wants.
    if (net->welcomed && !net->lost && !pl.dead) {
        Writer w;
        w.u8((uint8_t)Msg::Input);
        w.u16(++net->cmdSeq);
        w.f32(localCmd.aim);
        w.f32(localCmd.move);
        w.u8((localCmd.thrust ? 1 : 0) | (localCmd.fire ? 2 : 0));
        w.u8(localCmd.jumpSeq);
        w.u8(localCmd.heavySeq);
        net->ep.sendUnreliable(w.b);
    } else if (net->welcomed && !net->lost) {
        // Dead: still say something now and then, so the host knows we are here.
        Writer w;
        w.u8((uint8_t)Msg::Input);
        w.u16(++net->cmdSeq);
        w.f32(localCmd.aim);  w.f32(0.0f);  w.u8(0);
        w.u8(localCmd.jumpSeq);  w.u8(localCmd.heavySeq);
        net->ep.sendUnreliable(w.b);
    }
    net->ep.flush(net->now);

    // Bandwidth, averaged over a second.
    if (net->now - net->markAt >= 1.0) {
        const uint64_t total = net->link->bytesRecv;
        net->kbps = (double)(total - net->bytesAtMark) / 1024.0 / (net->now - net->markAt);
        net->bytesAtMark = total;
        net->markAt = net->now;
    }
}
