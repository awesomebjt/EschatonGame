#include "net/client.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "net/enet_util.h"
#include "world/wrap.h"

namespace net {

namespace {

constexpr double k_max_extrapolate = 0.25;   // s past the newest snapshot
constexpr double k_offset_smoothing = 0.1;

float lerp_angle(float a, float b, float t)
{
    float d = std::remainder(b - a, 6.28318530718f);
    return a + d * t;
}

PlayerState lerp_state(const PlayerState& a, const PlayerState& b, float t)
{
    PlayerState s = t < 0.5f ? a : b;   // flags and action from the nearer sample
    s.x     = a.x + world::wrap_dx(b.x - a.x) * t;
    s.y     = a.y + (b.y - a.y) * t;
    s.alt   = a.alt + (b.alt - a.alt) * t;
    s.yaw   = lerp_angle(a.yaw, b.yaw, t);
    s.pitch = a.pitch + (b.pitch - a.pitch) * t;
    for (int k = 0; k < 3; ++k) s.vel[k] = a.vel[k] + (b.vel[k] - a.vel[k]) * t;
    return s;
}

} // namespace

bool Client::connect(const std::string& host, uint16_t port, const Invite& invite)
{
    disconnect();
    if (!detail::ensure_enet()) return false;

    ENetAddress addr;
    if (enet_address_set_host(&addr, host.c_str()) != 0) return false;
    addr.port = port;

    m_host = enet_host_create(nullptr, 1, k_channel_count, 0, 0);
    if (!m_host) return false;
    m_peer = enet_host_connect(m_host, &addr, k_channel_count, 0);
    if (!m_peer) {
        enet_host_destroy(m_host);
        m_host = nullptr;
        return false;
    }
    m_invite    = invite;
    m_endpoint  = host + ":" + std::to_string(port);
    m_status    = Status::Connecting;
    m_reject    = Reject::None;
    m_color.reset();
    m_seq       = 0;
    m_next_send = 0;
    m_players   = {};
    m_last_tick = 0;
    m_snapshots = 0;
    return true;
}

void Client::disconnect()
{
    if (!m_host) return;
    if (m_peer && (m_status == Status::InSession || m_status == Status::Handshake)) {
        enet_peer_disconnect(m_peer, k_client_bye);
        ENetEvent ev;
        while (enet_host_service(m_host, &ev, 200) > 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
            else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) break;
        }
    }
    enet_host_destroy(m_host);
    m_host = nullptr;
    m_peer = nullptr;
    if (m_status != Status::Rejected) m_status = Status::Idle;
    m_players = {};
}

void Client::update(double now, const PlayerState* local)
{
    if (!m_host) return;

    ENetEvent ev;
    while (m_host && enet_host_service(m_host, &ev, 0) > 0) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT:
                enet_peer_timeout(ev.peer, 8, 3000, 8000);
                detail::send(ev.peer, k_channel_control, encode_hello(m_invite), true);
                m_status = Status::Handshake;
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                on_receive(ev.packet->data, ev.packet->dataLength, now);
                enet_packet_destroy(ev.packet);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                // A refused handshake arrives as a disconnect carrying the reason.
                m_reject = static_cast<Reject>(ev.data);
                m_status = m_reject == Reject::None || m_reject == Reject::ServerQuit ? Status::Lost
                         : Status::Rejected;
                enet_host_destroy(m_host);
                m_host = nullptr;
                m_peer = nullptr;
                m_players = {};
                return;
            default:
                break;
        }
    }

    if (m_status == Status::InSession && local && now >= m_next_send) {
        detail::send(m_peer, k_channel_state, encode_state(++m_seq, *local), false);
        m_next_send += 1.0 / k_send_hz;
        if (m_next_send < now) m_next_send = now + 1.0 / k_send_hz;
    }
    enet_host_flush(m_host);
}

void Client::on_receive(const uint8_t* data, size_t size, double now)
{
    Reader r(data, size);
    const auto type = static_cast<MsgType>(r.u8());

    if (type == MsgType::Welcome) {
        const uint8_t c = r.u8();
        if (!r.done() || c >= k_max_players) return;
        m_color  = static_cast<PlayerColor>(c);
        m_status = Status::InSession;
        return;
    }
    if (type != MsgType::Snapshot || m_status != Status::InSession) return;

    const uint32_t tick        = r.u32();
    const double   server_time = r.f64();
    const uint8_t  n           = r.u8();
    if (!r.ok() || n > k_max_players) return;
    SnapshotEntry entries[k_max_players];
    for (uint8_t i = 0; i < n; ++i) {
        const uint8_t c   = r.u8();
        const float   age = r.f32();
        entries[i]        = {static_cast<PlayerColor>(c), age, r.state()};
        if (c >= k_max_players || !(age >= 0.0f && age < 60.0f)) return;
    }
    if (!r.done()) return;
    if (m_snapshots && static_cast<int32_t>(tick - m_last_tick) <= 0) return;   // stale
    m_last_tick = tick;

    const double sample = server_time - now;
    m_clock_offset = m_snapshots == 0 ? sample : m_clock_offset + k_offset_smoothing * (sample - m_clock_offset);
    ++m_snapshots;

    bool seen[k_max_players] = {};
    for (uint8_t i = 0; i < n; ++i) {
        const int c = static_cast<int>(entries[i].color);
        if (m_color && c == static_cast<int>(*m_color)) continue;
        if (!state_plausible(entries[i].state)) continue;
        seen[c]    = true;
        History& h = m_players[c];
        h.present  = true;
        // A state relayed again (no new report since the last snapshot) is not
        // a new sample.
        const double when = server_time - entries[i].age;
        if (h.count && when <= h.ring[(h.count - 1) % h.ring.size()].time + 1e-6) continue;
        h.ring[h.count % h.ring.size()] = {when, entries[i].state};
        ++h.count;
    }
    // Absent from a snapshot means gone; a later return starts a fresh history.
    for (int c = 0; c < k_max_players; ++c)
        if (!seen[c]) m_players[c] = {};
}

std::optional<PlayerState> Client::remote(PlayerColor c, double now) const
{
    const History& h = m_players[static_cast<int>(c)];
    if (!h.present || h.count == 0) return std::nullopt;

    const double   t     = now + m_clock_offset - k_interp_delay;
    const uint32_t n     = std::min<uint32_t>(h.count, static_cast<uint32_t>(h.ring.size()));
    const uint32_t first = h.count - n;
    auto at = [&](uint32_t k) -> const Sample& { return h.ring[k % h.ring.size()]; };

    const Sample& newest = at(h.count - 1);
    if (t >= newest.time) {
        // Late: carry on along the last reported velocity, briefly.
        const double dt = std::min(t - newest.time, k_max_extrapolate);
        PlayerState  s  = newest.state;
        s.x   = world::wrap_x(s.x + s.vel[0] * dt);
        s.y  += s.vel[1] * dt;
        s.alt = std::max(s.alt + s.vel[2] * dt, s.alt - 0.5);
        return s;
    }
    for (uint32_t k = h.count - 1; k > first; --k) {
        const Sample& a = at(k - 1);
        const Sample& b = at(k);
        if (t >= a.time) {
            const double span = b.time - a.time;
            const float  u    = span > 1e-6 ? static_cast<float>((t - a.time) / span) : 1.0f;
            PlayerState  s    = lerp_state(a.state, b.state, u);
            s.x = world::wrap_x(s.x);
            return s;
        }
    }
    return at(first).state;   // older than the history: hold the oldest
}

std::string Client::describe() const
{
    char buf[160];
    switch (m_status) {
        case Status::Idle:       return "offline";
        case Status::Connecting: std::snprintf(buf, sizeof buf, "connecting to %s ...", m_endpoint.c_str()); break;
        case Status::Handshake:  std::snprintf(buf, sizeof buf, "joining %s ...", m_endpoint.c_str()); break;
        case Status::Rejected:
            std::snprintf(buf, sizeof buf, "refused by %s: %s", m_endpoint.c_str(), reject_text(m_reject));
            break;
        case Status::Lost:
            std::snprintf(buf, sizeof buf, "disconnected from %s: %s", m_endpoint.c_str(),
                          m_reject != Reject::None ? reject_text(m_reject)
                          : m_color                ? "connection lost"
                                                   : "no answer");
            break;
        case Status::InSession: {
            int others = 0;
            for (const History& h : m_players) others += h.present;
            std::snprintf(buf, sizeof buf, "%s as %s, %d other player%s", m_endpoint.c_str(),
                          m_color ? color_name(*m_color) : "?", others, others == 1 ? "" : "s");
            break;
        }
    }
    return buf;
}

} // namespace net
