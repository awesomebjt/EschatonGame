#pragma once

#include <cstdint>

namespace net {

// Runs a headless server on `port` until SIGINT/SIGTERM (Ctrl+C): prints the
// invite code and how to join, then one line per event. No window, GPU or
// world. Shared by `eschaton --server` and the standalone eschaton_server.
// Returns the process exit code.
int run_dedicated_server(uint16_t port);

} // namespace net
