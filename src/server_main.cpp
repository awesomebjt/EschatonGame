// eschaton_server: the multiplayer server on its own, without the renderer,
// SDL or the world, so it runs on a headless machine with nothing but the C and
// C++ runtimes. Same as `eschaton --server`.

#include <cstdio>
#include <cstring>

#include "net/dedicated.h"
#include "net/protocol.h"

int main(int argc, char* argv[])
{
    uint16_t port = net::k_default_port;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) {
            if (!net::parse_port(argv[++i], port)) {
                std::fprintf(stderr, "--port: not a port number: %s\n", argv[i]);
                return 1;
            }
        } else {
            std::fprintf(stderr, "usage: %s [--port n]   (default %u, UDP)\n", argv[0], unsigned(net::k_default_port));
            return std::strcmp(argv[i], "--help") ? 1 : 0;
        }
    }
    return net::run_dedicated_server(port);
}
