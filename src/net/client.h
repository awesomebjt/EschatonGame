#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "net/protocol.h"

typedef struct _ENetHost ENetHost;
typedef struct _ENetPeer ENetPeer;

namespace net {

// The game's side of a multiplayer session: connects, presents the invite code,
// streams the local player's state at k_send_hz and keeps a short history of
// every other player's state for interpolation.
//
// All ENet traffic runs on a thread of its own, started by connect(), so the
// connection survives the game's main loop stalling: a long load, a window the
// compositor stops presenting (minimised under Wayland blocks on vsync), a
// debugger. ENet drops a peer that goes 3-8 s unanswered. The public methods
// are for the game's thread and are safe against the network thread.
class Client
{
public:
    enum class Status { Idle, Connecting, Handshake, InSession, Rejected, Lost };

    ~Client() { disconnect(); }

    // Starts connecting; progress shows in status(). False if the host doesn't resolve.
    bool connect(const std::string& host, uint16_t port, const Invite& invite);
    // Graceful leave; waits up to ~200 ms for the server to acknowledge.
    void disconnect();

    // Hands over the local player's latest state (null before it exists); the
    // network thread sends the newest one at k_send_hz. `now` is the caller's
    // clock in seconds, monotonic, and is the clock remote() takes.
    void update(double now, const PlayerState* local);

    Status                     status() const;
    bool                       in_session() const { return status() == Status::InSession; }
    Reject                     reject_reason() const;
    std::optional<PlayerColor> color() const;
    const std::string&         endpoint() const { return m_endpoint; }   // set by connect() only
    bool                       has_snapshot() const { return snapshots() > 0; }
    uint32_t                   snapshots() const;

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

    // Network thread: services ENet and sends until m_stop or the link drops.
    void run();
    // Under m_mutex, on the network thread; `now` is the client's own clock.
    void on_receive(const uint8_t* data, size_t size, double now);
    static double clock();   // the client's own clock: steady, seconds

    // Owned by the network thread while it runs; by the caller otherwise.
    ENetHost*   m_host = nullptr;
    ENetPeer*   m_peer = nullptr;
    Invite      m_invite{};
    std::string m_endpoint;
    Status      m_status = Status::Idle;
    Reject      m_reject = Reject::None;
    std::optional<PlayerColor> m_color;

    uint32_t m_seq = 0;
    double   m_next_send = 0;
    uint32_t m_sent_version = 0;

    std::thread       m_thread;
    std::atomic<bool> m_stop{false};

    // Everything below, and m_status, m_reject and m_color above, is shared
    // between the threads and guarded by m_mutex.
    mutable std::mutex m_mutex;
    PlayerState        m_local;
    uint32_t           m_local_version = 0;   // bumped by every update() with a state
    double             m_caller_offset = 0;   // caller's clock minus ours

    std::array<History, k_max_players> m_players{};
    uint32_t m_last_tick = 0;
    uint32_t m_snapshots = 0;
    // Server clock minus ours, smoothed; maps our clock onto snapshot times.
    double   m_clock_offset = 0;
};

} // namespace net
