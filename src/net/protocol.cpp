#include "net/protocol.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <random>

namespace net {

const char* color_name(PlayerColor c)
{
    switch (c) {
        case PlayerColor::Blue:   return "BLUE";
        case PlayerColor::Green:  return "GREEN";
        case PlayerColor::Red:    return "RED";
        case PlayerColor::Yellow: return "YELLOW";
    }
    return "?";
}

std::array<float, 3> color_rgb(PlayerColor c)
{
    switch (c) {
        case PlayerColor::Blue:   return {0.16f, 0.36f, 0.95f};
        case PlayerColor::Green:  return {0.16f, 0.72f, 0.26f};
        case PlayerColor::Red:    return {0.90f, 0.14f, 0.12f};
        case PlayerColor::Yellow: return {0.96f, 0.80f, 0.10f};
    }
    return {1, 1, 1};
}

const char* reject_text(Reject r)
{
    switch (r) {
        case Reject::None:       return "disconnected";
        case Reject::BadInvite:  return "wrong invite code";
        case Reject::Full:       return "server is full";
        case Reject::Version:    return "protocol version mismatch";
        case Reject::Malformed:  return "malformed message";
        case Reject::Timeout:    return "handshake timed out";
        case Reject::ServerQuit: return "server shut down";
    }
    return "unknown reason";
}

Invite make_invite()
{
    // libstdc++, libc++ and MSVC all back random_device with the OS CSPRNG
    // (getrandom / arc4random / BCryptGenRandom).
    std::random_device rd;
    Invite code{};
    for (size_t i = 0; i < code.size(); i += 4) {
        const uint32_t v = rd();
        for (size_t k = 0; k < 4; ++k) code[i + k] = static_cast<uint8_t>(v >> (8 * k));
    }
    return code;
}

std::string invite_hex(const Invite& code)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (uint8_t b : code) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

std::optional<Invite> parse_invite(const char* text)
{
    Invite code{};
    size_t nibbles = 0;
    for (const char* c = text; *c; ++c) {
        if (*c == '-' || *c == ' ') continue;
        int d;
        if (*c >= '0' && *c <= '9')      d = *c - '0';
        else if (*c >= 'a' && *c <= 'f') d = *c - 'a' + 10;
        else if (*c >= 'A' && *c <= 'F') d = *c - 'A' + 10;
        else return std::nullopt;
        if (nibbles >= 2 * k_invite_bytes) return std::nullopt;
        code[nibbles / 2] = static_cast<uint8_t>(code[nibbles / 2] | (nibbles % 2 ? d : d << 4));
        ++nibbles;
    }
    if (nibbles != 2 * k_invite_bytes) return std::nullopt;
    return code;
}

bool invite_equal(const Invite& a, const Invite& b)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
    return diff == 0;
}

bool parse_endpoint(const char* text, std::string& host, uint16_t& port)
{
    const char* colon = std::strrchr(text, ':');
    port = k_default_port;
    if (!colon) {
        host = text;
        return !host.empty();
    }
    host.assign(text, colon);
    char* end = nullptr;
    const long p = std::strtol(colon + 1, &end, 10);
    if (host.empty() || !end || *end || p <= 0 || p > 65535) return false;
    port = static_cast<uint16_t>(p);
    return true;
}

bool state_plausible(const PlayerState& s)
{
    const double d[] = {s.x, s.y, s.alt, s.yaw, s.pitch, s.vel[0], s.vel[1], s.vel[2]};
    for (double v : d)
        if (!std::isfinite(v)) return false;
    // Bounds well outside any real map, only to keep garbage out of the ledger.
    return std::fabs(s.x) < 1e6 && std::fabs(s.y) < 1e6 && std::fabs(s.alt) < 1e5
        && std::fabs(s.vel[0]) < 1e4 && std::fabs(s.vel[1]) < 1e4 && std::fabs(s.vel[2]) < 1e4
        && static_cast<uint8_t>(s.action) <= static_cast<uint8_t>(Action::UseItem);
}

// -- Writer --------------------------------------------------------------------

void Writer::u16(uint16_t v)
{
    u8(static_cast<uint8_t>(v));
    u8(static_cast<uint8_t>(v >> 8));
}

void Writer::u32(uint32_t v)
{
    for (int k = 0; k < 4; ++k) u8(static_cast<uint8_t>(v >> (8 * k)));
}

void Writer::f32(float v)
{
    uint32_t u;
    std::memcpy(&u, &v, 4);
    u32(u);
}

void Writer::f64(double v)
{
    uint64_t u;
    std::memcpy(&u, &v, 8);
    for (int k = 0; k < 8; ++k) u8(static_cast<uint8_t>(u >> (8 * k)));
}

void Writer::state(const PlayerState& s)
{
    f64(s.x);
    f64(s.y);
    f64(s.alt);
    f32(s.yaw);
    f32(s.pitch);
    for (float v : s.vel) f32(v);
    u8(s.flags);
    u8(static_cast<uint8_t>(s.action));
    u8(s.action_seq);
}

// -- Reader --------------------------------------------------------------------

uint8_t Reader::u8()
{
    if (!good || left < 1) { good = false; return 0; }
    --left;
    return *p++;
}

uint16_t Reader::u16()
{
    const uint16_t a = u8();
    return static_cast<uint16_t>(a | (u8() << 8));
}

uint32_t Reader::u32()
{
    uint32_t v = 0;
    for (int k = 0; k < 4; ++k) v |= uint32_t(u8()) << (8 * k);
    return v;
}

float Reader::f32()
{
    const uint32_t u = u32();
    float v;
    std::memcpy(&v, &u, 4);
    return v;
}

double Reader::f64()
{
    uint64_t u = 0;
    for (int k = 0; k < 8; ++k) u |= uint64_t(u8()) << (8 * k);
    double v;
    std::memcpy(&v, &u, 8);
    return v;
}

void Reader::raw(uint8_t* out, size_t n)
{
    if (!good || left < n) { good = false; std::memset(out, 0, n); return; }
    std::memcpy(out, p, n);
    p += n;
    left -= n;
}

PlayerState Reader::state()
{
    PlayerState s;
    s.x     = f64();
    s.y     = f64();
    s.alt   = f64();
    s.yaw   = f32();
    s.pitch = f32();
    for (float& v : s.vel) v = f32();
    s.flags      = u8();
    s.action     = static_cast<Action>(u8());
    s.action_seq = u8();
    return s;
}

// -- Messages --------------------------------------------------------------------

std::vector<uint8_t> encode_hello(const Invite& code)
{
    Writer w;
    w.u8(static_cast<uint8_t>(MsgType::Hello));
    w.u32(k_magic);
    w.u16(k_protocol_version);
    w.raw(code.data(), code.size());
    return std::move(w.bytes);
}

std::vector<uint8_t> encode_welcome(PlayerColor color)
{
    Writer w;
    w.u8(static_cast<uint8_t>(MsgType::Welcome));
    w.u8(static_cast<uint8_t>(color));
    return std::move(w.bytes);
}

std::vector<uint8_t> encode_state(uint32_t seq, const PlayerState& s)
{
    Writer w;
    w.u8(static_cast<uint8_t>(MsgType::State));
    w.u32(seq);
    w.state(s);
    return std::move(w.bytes);
}

std::vector<uint8_t> encode_snapshot(uint32_t tick, double server_time, const SnapshotEntry* entries, size_t count)
{
    Writer w;
    w.u8(static_cast<uint8_t>(MsgType::Snapshot));
    w.u32(tick);
    w.f64(server_time);
    w.u8(static_cast<uint8_t>(count));
    for (size_t i = 0; i < count; ++i) {
        w.u8(static_cast<uint8_t>(entries[i].color));
        w.f32(entries[i].age);
        w.state(entries[i].state);
    }
    return std::move(w.bytes);
}

} // namespace net
