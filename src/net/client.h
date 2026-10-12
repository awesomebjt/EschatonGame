#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "net/protocol.h"

typedef struct _ENetHost ENetHost;
typedef struct _ENetPeer ENetPeer;

namespace net {

// The game's side of a multiplayer session: connects, presents the invite code,
// streams the local player's state at k_send_hz and keeps a short history of
// every other player's state for interpolation.
class Client
{
public:
    enum class Status { Idle, Connecting, Handshake, InSession, Rejected, Lost };

    ~Client() { disconnect(); }

    // Starts connecting; progress shows in status(). False if the host doesn't resolve.
    bool connect(const std::string& host, uint16_t port, const Invite& invite);
    // Graceful leave; waits up to ~200 ms for the server to acknowledge.
    void disconnect();

    // Handles network events and, in session, sends `local` if a send is due
    // (pass null before the local player exists). `now` is the caller's clock in
    // seconds; it must be monotonic.
    void update(double now, const PlayerState* local);

    Status                     status() const { return m_status; }
    bool                       in_session() const { return m_status == Status::InSession; }
    Reject                     reject_reason() const { return m_reject; }
    std::optional<PlayerColor> color() const { return m_color; }
    const std::string&         endpoint() const { return m_endpoint; }
    bool                       has_snapshot() const { return m_snapshots > 0; }
    uint32_t                   snapshots() const { return m_snapshots; }

    // Another player's state as of `now` minus k_interp_delay, interpolated
    // between snapshots (extrapolated briefly from velocity if they run late).
    // Empty if that colour isn't playing, or is this client.
    std::optional<PlayerState> remote(PlayerColor c, double now) const;

    // One line for the HUD: "connected as RED, 3 players", "connecting to ...".
    std::string describe() const;

private:
    struct Sample
    {
        double      time;   // server clock
        PlayerState state;
    };
    struct History
    {
        bool                  present = false;
        std::array<Sample, 8> ring{};
        uint32_t              count = 0;   // samples written, ever
    };

    void on_receive(const uint8_t* data, size_t size, double now);

    ENetHost*   m_host = nullptr;
    ENetPeer*   m_peer = nullptr;
    Invite      m_invite{};
    std::string m_endpoint;
    Status      m_status = Status::Idle;
    Reject      m_reject = Reject::None;
    std::optional<PlayerColor> m_color;

    uint32_t m_seq = 0;
    double   m_next_send = 0;

    std::array<History, k_max_players> m_players{};
    uint32_t m_last_tick = 0;
    uint32_t m_snapshots = 0;
    // Server clock minus ours, smoothed; maps our clock onto snapshot times.
    double   m_clock_offset = 0;
};

} // namespace net
