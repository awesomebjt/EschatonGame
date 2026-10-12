#pragma once

// Shared by server.cpp and client.cpp only; keeps <enet/enet.h> out of the
// public headers.

#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>

#include <enet/enet.h>

namespace net::detail {

// enet_initialize is process-wide; the test runs a server and clients together.
inline bool ensure_enet()
{
    static std::once_flag once;
    static bool           ok = false;
    std::call_once(once, [] {
        ok = enet_initialize() == 0;
        if (ok) std::atexit(enet_deinitialize);
    });
    return ok;
}

inline void send(ENetPeer* peer, uint8_t channel, const std::vector<uint8_t>& bytes, bool reliable)
{
    ENetPacket* p = enet_packet_create(bytes.data(), bytes.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (enet_peer_send(peer, channel, p) != 0) enet_packet_destroy(p);
}

} // namespace net::detail
