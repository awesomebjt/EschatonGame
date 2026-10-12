#pragma once

#include <array>
#include <cstdint>
#include <cstdio>

#include "net/protocol.h"

typedef struct _ENetHost ENetHost;
typedef struct _ENetPeer ENetPeer;

namespace net {

// One seat in the server's world-state ledger. The seat index is the colour.
struct LedgerEntry
{
    bool        connected = false;   // past the handshake
    bool        has_state = false;   // has reported at least once
    PlayerState state;
    uint32_t    last_seq    = 0;
    double      last_update = 0;     // server time of the last report
    ENetPeer*   peer        = nullptr;
};

// The headless server: authenticates clients by invite code, gives each a free
// colour at random, keeps the latest state each one reports, and broadcasts the
// whole ledger k_snapshot_hz times a second. It holds no world: positions are
// relayed as reported, so it needs neither the map nor a GPU.
class Server
{
public:
    ~Server() { stop(); }

    // Listens on every IPv4 interface. `log` gets one line per event (or null).
    bool start(uint16_t port, const Invite& invite, std::FILE* log);
    void stop();

    // Handles network events for up to `timeout_ms`, then sends a snapshot if one
    // is due. Call in a loop.
    void service(uint32_t timeout_ms);

    uint16_t port() const { return m_port; }
    int      player_count() const;
    const std::array<LedgerEntry, k_max_players>& ledger() const { return m_ledger; }

private:
    struct Pending
    {
        ENetPeer* peer = nullptr;
        double    since = 0;
    };

    double now() const;
    void   on_receive(ENetPeer* peer, const uint8_t* data, size_t size);
    void   on_hello(ENetPeer* peer, Reader& r);
    void   on_leave(ENetPeer* peer, const char* why);
    void   broadcast_snapshot();
    void   log(const char* fmt, ...) const;

    ENetHost*                              m_host = nullptr;
    Invite                                 m_invite{};
    uint16_t                               m_port = 0;
    std::FILE*                             m_log  = nullptr;
    std::array<LedgerEntry, k_max_players> m_ledger{};
    std::array<Pending, 2 * k_max_players> m_pending{};   // connected, no Hello yet
    uint32_t                               m_tick = 0;
    double                                 m_next_snapshot = 0;
    double                                 m_epoch = 0;
    uint32_t                               m_rng = 0;
};

} // namespace net
