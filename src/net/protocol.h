#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Wire protocol shared by the headless server and the game client. ENet carries
// it over UDP: channel 0 is reliable and ordered (handshake), channel 1 is
// unreliable and sequenced (player states and snapshots, where only the newest
// matters). Every message starts with a MsgType byte; numbers are little-endian.
namespace net {

inline constexpr uint16_t k_default_port     = 47210;
inline constexpr uint32_t k_magic            = 0x48435345;   // "ESCH" as little-endian bytes
inline constexpr uint16_t k_protocol_version = 1;
inline constexpr int      k_max_players      = 4;
inline constexpr size_t   k_invite_bytes     = 16;           // 128 bits: not guessable

// How often clients report their state and the server broadcasts the ledger.
// Remote players are drawn k_interp_delay in the past, between two snapshots.
inline constexpr double k_send_hz      = 30.0;
inline constexpr double k_snapshot_hz  = 30.0;
inline constexpr double k_interp_delay = 0.1;

enum Channel : uint8_t
{
    k_channel_control = 0,   // reliable
    k_channel_state   = 1,   // unreliable, sequenced
    k_channel_count
};

// Each player is a colour; there are as many colours as seats, so the colour is
// also the ledger slot and the player's name.
enum class PlayerColor : uint8_t { Blue, Green, Red, Yellow };

const char* color_name(PlayerColor c);                 // "BLUE", ...
std::array<float, 3> color_rgb(PlayerColor c);        // linear-ish display colour

// What a player is doing besides moving. Only None is sent today; the field and
// its sequence number exist so later actions (interacting, using items, emotes)
// need no protocol change. States are sent continuously and may be dropped, so
// a one-shot action is recognised by action_seq changing, not by seeing the
// action once.
enum class Action : uint8_t
{
    None     = 0,
    Jump     = 1,
    Interact = 2,
    UseItem  = 3,
};

enum StateFlags : uint8_t
{
    k_flag_grounded = 1 << 0,
    k_flag_noclip   = 1 << 1,
};

// One player's state as clients report it and the server relays it. Map space:
// x around the circumference (wraps), y along the axis, alt off the wall; the
// position is the feet (the bottom of the collision capsule).
struct PlayerState
{
    double  x = 0, y = 0, alt = 0;
    float   yaw = 0, pitch = 0;        // radians; yaw 0 looks along +y
    float   vel[3] = {0, 0, 0};        // m/s along (spinward, axial, up)
    uint8_t flags = 0;                 // StateFlags
    Action  action = Action::None;
    uint8_t action_seq = 0;            // bumped every time a new action starts
};

enum class MsgType : uint8_t
{
    Hello    = 1,   // client → server: magic, version, invite code
    Welcome  = 2,   // server → client: assigned colour
    State    = 3,   // client → server: sequence number, PlayerState
    Snapshot = 4,   // server → client: tick, server time, every player's state and its age
};

// Sent as the ENet disconnect data when the server turns a client away.
enum class Reject : uint32_t
{
    None       = 0,
    BadInvite  = 1,
    Full       = 2,
    Version    = 3,
    Malformed  = 4,
    Timeout    = 5,   // no Hello in time
    ServerQuit = 6,
};
const char* reject_text(Reject r);

// Disconnect data a client sends when it leaves on purpose; a timeout carries 0.
inline constexpr uint32_t k_client_bye = 0xB1E;

using Invite = std::array<uint8_t, k_invite_bytes>;

Invite                make_invite();                      // from the OS entropy source
std::string           invite_hex(const Invite& code);     // 32 lowercase hex digits
std::optional<Invite> parse_invite(const char* text);     // ignores '-' and spaces; any case
bool                  invite_equal(const Invite& a, const Invite& b);   // constant time

// Parses "host", "host:port" (port defaults to k_default_port).
bool parse_endpoint(const char* text, std::string& host, uint16_t& port);

// Sanity limits on a reported state: finite numbers, inside generous map bounds.
bool state_plausible(const PlayerState& s);

// -- Encoding -----------------------------------------------------------------

struct Writer
{
    std::vector<uint8_t> bytes;
    void u8(uint8_t v) { bytes.push_back(v); }
    void u16(uint16_t v);
    void u32(uint32_t v);
    void f32(float v);
    void f64(double v);
    void raw(const uint8_t* p, size_t n) { bytes.insert(bytes.end(), p, p + n); }
    void state(const PlayerState& s);
};

// Reads fail softly: once short, every read returns zero and ok() stays false.
struct Reader
{
    const uint8_t* p;
    size_t         left;
    bool           good = true;

    Reader(const uint8_t* data, size_t size) : p(data), left(size) {}
    bool     ok() const { return good; }
    bool     done() const { return good && left == 0; }
    uint8_t  u8();
    uint16_t u16();
    uint32_t u32();
    float    f32();
    double   f64();
    void     raw(uint8_t* out, size_t n);
    PlayerState state();
};

struct SnapshotEntry
{
    PlayerColor color;
    float       age = 0;   // seconds between the server receiving this state and sending the snapshot
    PlayerState state;
};

std::vector<uint8_t> encode_hello(const Invite& code);
std::vector<uint8_t> encode_welcome(PlayerColor color);
std::vector<uint8_t> encode_state(uint32_t seq, const PlayerState& s);
std::vector<uint8_t> encode_snapshot(uint32_t tick, double server_time, const SnapshotEntry* entries, size_t count);

} // namespace net
