#include "net/dedicated.h"

#include <csignal>
#include <cstdio>

#include "net/protocol.h"
#include "net/server.h"

namespace net {

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_stop_signal(int) { g_stop = 1; }

} // namespace

int run_dedicated_server(uint16_t port)
{
    // Events show up promptly when piped. MSVC has no line buffering and treats
    // _IOLBF with a zero size as an invalid parameter (abort), so go unbuffered.
#ifdef _WIN32
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#else
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
#endif
    const Invite invite = make_invite();
    Server       server;
    if (!server.start(port, invite, stdout)) return 1;

    std::printf("Eschaton server listening on UDP port %u (up to %d players)\n"
                "invite code: %s\n"
                "join with:   eschaton --connect <this-host>:%u %s\n"
                "Ctrl+C stops the server.\n",
                unsigned(port), k_max_players, invite_hex(invite).c_str(), unsigned(port),
                invite_hex(invite).c_str());

    std::signal(SIGINT, on_stop_signal);
    std::signal(SIGTERM, on_stop_signal);
    while (!g_stop) server.service(10);
    std::printf("shutting down\n");
    server.stop();
    return 0;
}

} // namespace net
