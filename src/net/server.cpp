#include "net/server.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <random>

#include "net/enet_util.h"

namespace net {

namespace {

constexpr double k_hello_timeout = 5.0;   // s from connect to a valid Hello

// ENet peer user data: seat index + 1 once admitted, null while pending.
int seat_of(const ENetPeer* peer)
{
    return static_cast<int>(reinterpret_cast<intptr_t>(peer->data)) - 1;
}

} // namespace

double Server::now() const
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count() - m_epoch;
}

void Server::log(const char* fmt, ...) const
{
    if (!m_log) return;
    va_list args;
    va_start(args, fmt);
    std::fputs("[server] ", m_log);
    std::vfprintf(m_log, fmt, args);
    std::fputc('\n', m_log);
    std::fflush(m_log);
    va_end(args);
}

bool Server::start(uint16_t port, const Invite& invite, std::FILE* log_file)
{
    stop();
    m_log = log_file;
    if (!detail::ensure_enet()) {
        log("enet_initialize failed");
        return false;
    }
    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    addr.port = port;
    // Twice as many peers as seats, so a fifth player gets a "server is full"
    // answer instead of a connection that never completes.
    m_host = enet_host_create(&addr, m_pending.size(), k_channel_count, 0, 0);
    if (!m_host) {
        log("cannot listen on UDP port %u (in use?)", unsigned(port));
        return false;
    }
    m_invite = invite;
    m_port   = port;
    m_ledger = {};
    m_pending = {};
    m_tick   = 0;
    using namespace std::chrono;
    m_epoch  = duration<double>(steady_clock::now().time_since_epoch()).count();
    m_next_snapshot = 0;
    m_rng = std::random_device{}() | 1u;
    return true;
}

void Server::stop()
{
    if (!m_host) return;
    // Tell everyone, then give the notices a moment to leave.
    for (size_t i = 0; i < m_host->peerCount; ++i) {
        ENetPeer* p = &m_host->peers[i];
        if (p->state == ENET_PEER_STATE_CONNECTED)
            enet_peer_disconnect(p, static_cast<uint32_t>(Reject::ServerQuit));
    }
    enet_host_flush(m_host);
    ENetEvent ev;
    while (enet_host_service(m_host, &ev, 100) > 0)
        if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
    enet_host_destroy(m_host);
    m_host = nullptr;
}

int Server::player_count() const
{
    int n = 0;
    for (const LedgerEntry& e : m_ledger) n += e.connected;
    return n;
}

void Server::service(uint32_t timeout_ms)
{
    if (!m_host) return;

    // Wait no later than the next snapshot.
    const double   t      = now();
    const double   until  = m_next_snapshot - t;
    const uint32_t wait   = until <= 0 ? 0u : std::min<uint32_t>(timeout_ms, static_cast<uint32_t>(until * 1000.0));
    ENetEvent ev;
    int r = enet_host_service(m_host, &ev, wait);
    while (r > 0) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                ev.peer->data = nullptr;
                enet_peer_timeout(ev.peer, 8, 3000, 8000);
                m_pending[ev.peer - m_host->peers] = {ev.peer, now()};
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE:
                on_receive(ev.peer, ev.packet->data, ev.packet->dataLength);
                enet_packet_destroy(ev.packet);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                on_leave(ev.peer, ev.data == k_client_bye ? "left" : "timed out");
                break;
            default:
                break;
        }
        r = enet_host_check_events(m_host, &ev);
    }

    // Drop connections that never introduce themselves.
    const double t2 = now();
    for (Pending& p : m_pending) {
        if (p.peer && t2 - p.since > k_hello_timeout) {
            enet_peer_disconnect(p.peer, static_cast<uint32_t>(Reject::Timeout));
            p = {};
        }
    }

    if (t2 >= m_next_snapshot) {
        broadcast_snapshot();
        // Keep the cadence even if a service call ran long, but never burst.
        m_next_snapshot += 1.0 / k_snapshot_hz;
        if (m_next_snapshot < t2) m_next_snapshot = t2 + 1.0 / k_snapshot_hz;
    }
    enet_host_flush(m_host);
}

void Server::on_receive(ENetPeer* peer, const uint8_t* data, size_t size)
{
    Reader r(data, size);
    const auto type = static_cast<MsgType>(r.u8());
    const int  seat = seat_of(peer);

    if (seat < 0) {
        // Not admitted: the only thing worth reading is a Hello.
        if (type == MsgType::Hello) on_hello(peer, r);
        else enet_peer_disconnect(peer, static_cast<uint32_t>(Reject::Malformed));
        return;
    }
    if (type != MsgType::State) return;   // nothing else is expected from a player

    const uint32_t    seq = r.u32();
    const PlayerState s   = r.state();
    if (!r.done() || !state_plausible(s)) return;
    LedgerEntry& e = m_ledger[seat];
    // Channel 1 is sequenced already; this also covers a wrapped counter.
    if (e.has_state && static_cast<int32_t>(seq - e.last_seq) <= 0) return;
    e.state       = s;
    e.has_state   = true;
    e.last_seq    = seq;
    e.last_update = now();
}

void Server::on_hello(ENetPeer* peer, Reader& r)
{
    const uint32_t magic   = r.u32();
    const uint16_t version = r.u16();
    Invite code{};
    r.raw(code.data(), code.size());
    m_pending[peer - m_host->peers] = {};

    char addr[64] = "?";
    enet_address_get_host_ip(&peer->address, addr, sizeof addr);

    Reject why = Reject::None;
    if (!r.done() || magic != k_magic)      why = Reject::Malformed;
    else if (version != k_protocol_version) why = Reject::Version;
    else if (!invite_equal(code, m_invite)) why = Reject::BadInvite;

    int free_seats[k_max_players];
    int n_free = 0;
    for (int i = 0; i < k_max_players; ++i)
        if (!m_ledger[i].connected) free_seats[n_free++] = i;
    if (why == Reject::None && n_free == 0) why = Reject::Full;

    if (why != Reject::None) {
        log("turned away %s:%u: %s", addr, unsigned(peer->address.port), reject_text(why));
        enet_peer_disconnect(peer, static_cast<uint32_t>(why));
        return;
    }

    // xorshift32: the colour only needs to look random, not be secret.
    m_rng ^= m_rng << 13;
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    const int seat = free_seats[m_rng % static_cast<uint32_t>(n_free)];

    LedgerEntry& e = m_ledger[seat];
    e           = {};
    e.connected = true;
    e.peer      = peer;
    peer->data  = reinterpret_cast<void*>(static_cast<intptr_t>(seat + 1));
    detail::send(peer, k_channel_control, encode_welcome(static_cast<PlayerColor>(seat)), true);
    log("%s joined from %s:%u (%d/%d players)", color_name(static_cast<PlayerColor>(seat)), addr,
        unsigned(peer->address.port), player_count(), k_max_players);
}

void Server::on_leave(ENetPeer* peer, const char* why)
{
    m_pending[peer - m_host->peers] = {};
    const int seat = seat_of(peer);
    peer->data = nullptr;
    if (seat < 0) return;
    m_ledger[seat] = {};
    log("%s %s (%d/%d players)", color_name(static_cast<PlayerColor>(seat)), why, player_count(), k_max_players);
}

void Server::broadcast_snapshot()
{
    // Each state goes out stamped with when it arrived, not when the snapshot
    // leaves: reports and snapshots aren't in step, and stamping them all "now"
    // would jitter the timeline by up to a send interval.
    const double  t = now();
    SnapshotEntry entries[k_max_players];
    size_t        n = 0;
    for (int i = 0; i < k_max_players; ++i)
        if (m_ledger[i].connected && m_ledger[i].has_state)
            entries[n++] = {static_cast<PlayerColor>(i), static_cast<float>(t - m_ledger[i].last_update),
                            m_ledger[i].state};

    ++m_tick;
    const std::vector<uint8_t> bytes = encode_snapshot(m_tick, t, entries, n);
    for (const LedgerEntry& e : m_ledger)
        if (e.connected) detail::send(e.peer, k_channel_state, bytes, false);
}

} // namespace net
