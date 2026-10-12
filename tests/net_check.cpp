// Headless multiplayer checks: a server and bot clients in one process, talking
// over loopback. Single-threaded: every loop pumps the server and each client.
//
//   ./build/net_check                          run the checks
//   ./build/net_check --bot host[:port] invite x y [seconds]
//                                              join a real server as a bot that
//                                              walks a 3 m circle around (x, y)

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include "net/client.h"
#include "net/protocol.h"
#include "net/server.h"
#include "world/wrap.h"

namespace {

constexpr uint16_t k_port = 47299;

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) ++g_failures;
}

double now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Where bot `i` is at time t: a 3 m circle at 1 rad/s (3 m/s), each bot on its own.
net::PlayerState bot_state(int i, double t, double cx = 1000, double cy = 2000)
{
    net::PlayerState s;
    const double a = t + i * 1.7;
    s.x      = world::wrap_x(cx + 10 * i + 3 * std::cos(a));
    s.y      = cy + 3 * std::sin(a);
    s.alt    = 0;
    s.vel[0] = static_cast<float>(-3 * std::sin(a));
    s.vel[1] = static_cast<float>(3 * std::cos(a));
    s.yaw    = static_cast<float>(std::atan2(s.vel[0], s.vel[1]));
    s.flags  = net::k_flag_grounded;
    return s;
}

struct Bot
{
    int                          id;
    std::unique_ptr<net::Client> client = std::make_unique<net::Client>();
};

// Runs everything for `seconds`, bots reporting their circle as they go.
void pump(net::Server& server, std::vector<Bot>& bots, double seconds, double t0,
          const std::function<void(double)>& each = {})
{
    const double end = now() + seconds;
    while (now() < end) {
        server.service(1);
        const double t = now();
        for (Bot& b : bots) {
            const net::PlayerState s = bot_state(b.id, t - t0);
            b.client->update(t, &s);
        }
        if (each) each(t);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void check_codec()
{
    const net::Invite code = net::make_invite();
    const std::string hex  = net::invite_hex(code);
    check(hex.size() == 32, "invite code is 32 hex digits");
    auto back = net::parse_invite(hex.c_str());
    check(back && net::invite_equal(*back, code), "invite code round-trips");
    std::string dashed = hex.substr(0, 8) + "-" + hex.substr(8, 8) + " " + hex.substr(16);
    for (char& c : dashed) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    back = net::parse_invite(dashed.c_str());
    check(back && net::invite_equal(*back, code), "invite code parses with dashes, spaces, upper case");
    check(!net::parse_invite("abc") && !net::parse_invite((hex + "0").c_str())
              && !net::parse_invite((hex.substr(1) + "g").c_str()),
          "short, long and non-hex invite codes are refused");
    check(net::make_invite() != code, "two invite codes differ");

    std::string host;
    uint16_t    port = 0;
    check(net::parse_endpoint("example.org:1234", host, port) && host == "example.org" && port == 1234
              && net::parse_endpoint("10.0.0.2", host, port) && host == "10.0.0.2" && port == net::k_default_port
              && !net::parse_endpoint("host:0", host, port) && !net::parse_endpoint("host:x", host, port),
          "host[:port] parsing");

    net::PlayerState s = bot_state(2, 0.3);
    s.pitch      = -0.4f;
    s.action     = net::Action::Interact;
    s.action_seq = 7;
    const std::vector<uint8_t> bytes = net::encode_state(42, s);
    net::Reader r(bytes.data(), bytes.size());
    const bool type_ok = r.u8() == static_cast<uint8_t>(net::MsgType::State);
    const uint32_t         seq = r.u32();
    const net::PlayerState d   = r.state();
    check(type_ok && seq == 42 && r.done() && d.x == s.x && d.y == s.y && d.yaw == s.yaw && d.pitch == s.pitch
              && d.vel[1] == s.vel[1] && d.flags == s.flags && d.action == s.action && d.action_seq == 7,
          "state message round-trips (with an action)");
    net::Reader short_r(bytes.data(), bytes.size() - 1);
    short_r.u8();
    short_r.u32();
    short_r.state();
    check(!short_r.ok(), "truncated message is detected");
    s.x = std::nan("");
    check(!net::state_plausible(s), "NaN state is implausible");
}

} // namespace

static int run_bot(int argc, char** argv)
{
    std::string host;
    uint16_t    port;
    if (argc < 6 || !net::parse_endpoint(argv[2], host, port)) return 2;
    const auto code = net::parse_invite(argv[3]);
    if (!code) return 2;
    const double cx = std::atof(argv[4]), cy = std::atof(argv[5]);
    const double secs = argc > 6 ? std::atof(argv[6]) : 60.0;
    // Stand still facing +y unless asked to walk.
    const bool walk = std::getenv("BOT_WALK") != nullptr;

    net::Client client;
    if (!client.connect(host, port, *code)) return 1;
    const double t0 = now();
    while (now() - t0 < secs) {
        net::PlayerState s = bot_state(0, now() - t0, cx, cy);
        if (!walk) {
            s = {};
            s.x = cx;
            s.y = cy;
            s.flags = net::k_flag_grounded;
        }
        client.update(now(), &s);
        if (client.status() == net::Client::Status::Rejected || client.status() == net::Client::Status::Lost) {
            std::printf("bot: %s\n", client.describe().c_str());
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::printf("bot: %s\n", client.describe().c_str());
    return 0;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc > 1 && !std::strcmp(argv[1], "--bot")) return run_bot(argc, argv);

    check_codec();

    net::Server       server;
    const net::Invite invite = net::make_invite();
    std::FILE*        log    = std::getenv("NET_LOG") ? stdout : nullptr;
    if (!server.start(k_port, invite, log)) {
        std::printf("FAIL  server could not listen on %u\n", unsigned(k_port));
        return 1;
    }
    const double t0 = now();
    std::vector<Bot> bots;

    // A wrong code is turned away.
    {
        std::vector<Bot> intruder(1);
        intruder[0].id = 9;
        net::Invite wrong = invite;
        wrong[5] ^= 1;
        intruder[0].client->connect("127.0.0.1", k_port, wrong);
        pump(server, intruder, 0.5, t0);
        check(intruder[0].client->status() == net::Client::Status::Rejected
                  && intruder[0].client->reject_reason() == net::Reject::BadInvite,
              "wrong invite code is refused");
        check(server.player_count() == 0, "refused client takes no seat");
    }

    // Four players fill the server with four different colours.
    for (int i = 0; i < 4; ++i) {
        bots.push_back({i});
        bots.back().client->connect("127.0.0.1", k_port, invite);
    }
    pump(server, bots, 0.5, t0);
    std::set<int> colors;
    bool all_in = true;
    for (Bot& b : bots) {
        all_in = all_in && b.client->in_session();
        if (b.client->color()) colors.insert(static_cast<int>(*b.client->color()));
    }
    check(all_in && server.player_count() == 4, "four players join");
    check(colors.size() == 4, "each player gets a different colour");

    // A fifth is told the server is full.
    {
        std::vector<Bot> fifth(1);
        fifth[0].id = 4;
        fifth[0].client->connect("127.0.0.1", k_port, invite);
        pump(server, fifth, 0.5, t0);
        check(fifth[0].client->status() == net::Client::Status::Rejected
                  && fifth[0].client->reject_reason() == net::Reject::Full,
              "fifth player is told the server is full");
    }

    // Everyone sees everyone else where they were k_interp_delay ago, and the
    // motion between frames is smooth (no step longer than the speed allows).
    double max_err = 0, max_step = 0;
    int    seen_ok = 0, samples = 0;
    std::vector<std::array<double, 2>> last(16, {NAN, NAN});
    const double measure_from = now() + 0.4;   // let the histories fill
    pump(server, bots, 1.5, t0, [&](double t) {
        if (t < measure_from) return;
        for (size_t a = 0; a < bots.size(); ++a) {
            int seen = 0;
            for (size_t b = 0; b < bots.size(); ++b) {
                if (a == b) continue;
                const auto st = bots[a].client->remote(*bots[b].client->color(), t);
                if (!st) continue;
                ++seen;
                const net::PlayerState want = bot_state(bots[b].id, t - t0 - net::k_interp_delay);
                max_err = std::max(max_err, std::hypot(world::wrap_dx(st->x - want.x), st->y - want.y));
                auto& prev = last[a * 4 + b];
                if (!std::isnan(prev[0]))
                    max_step = std::max(max_step, std::hypot(world::wrap_dx(st->x - prev[0]), st->y - prev[1]));
                prev = {st->x, st->y};
            }
            seen_ok += seen == 3;
            ++samples;
        }
    });
    std::printf("      remote error %.3f m, largest per-frame step %.3f m\n", max_err, max_step);
    check(samples > 0 && seen_ok == samples, "every player sees the other three");
    check(max_err < 0.35, "remote positions track the senders (< 0.35 m off)");
    // Frames here are ~3 ms apart at 3 m/s: 1 cm per frame; allow plenty.
    check(max_step < 0.1, "remote motion has no jumps between frames");

    // One leaves; the others stop seeing them and the seat frees up.
    const net::PlayerColor gone = *bots[1].client->color();
    bots[1].client->disconnect();
    bots.erase(bots.begin() + 1);
    pump(server, bots, 0.5, t0);
    bool vanished = true;
    for (Bot& b : bots) vanished = vanished && !b.client->remote(gone, now());
    check(server.player_count() == 3 && vanished, "a player who leaves disappears for the others");

    bots.push_back({7});
    bots.back().client->connect("127.0.0.1", k_port, invite);
    pump(server, bots, 0.5, t0);
    check(bots.back().client->in_session() && bots.back().client->color() == gone,
          "a new player takes the free colour");

    // A game that stalls (a load, a window the compositor stops presenting, a
    // debugger) stops calling update(); its network thread keeps the link up past
    // ENet's 8 s worst-case timeout, and the others still see it.
    {
        const net::PlayerColor stalled = *bots[0].client->color();
        std::vector<Bot> others;
        for (size_t i = 1; i < bots.size(); ++i) others.push_back(std::move(bots[i]));
        pump(server, others, 9.0, t0);
        bool still_seen = true;
        for (Bot& b : others) still_seen = still_seen && b.client->remote(stalled, now());
        for (size_t i = 1; i < bots.size(); ++i) bots[i] = std::move(others[i - 1]);
        check(bots[0].client->in_session() && server.player_count() == 4 && still_seen,
              "a client whose game stalls for 9 s keeps its seat");
    }

    // Shutting the server down tells the clients.
    server.stop();
    pump(server, bots, 0.3, t0);
    check(bots[0].client->status() == net::Client::Status::Lost
              && bots[0].client->reject_reason() == net::Reject::ServerQuit,
          "clients hear that the server shut down");

    std::printf(g_failures ? "%d check(s) failed\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
