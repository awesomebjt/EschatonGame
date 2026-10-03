#pragma once

#include <cstdint>

namespace render { struct CameraPos; class WorldRenderer; }

namespace debug {

class Console;

// Game state the console's Ruby commands may read and change. Pointers into
// main.cpp's globals; they must outlive the console.
struct DebugHost
{
    render::CameraPos*     cam       = nullptr;
    float*                 yaw       = nullptr;   // radians
    float*                 pitch     = nullptr;   // radians, clamped to ±pitch_max
    float                  pitch_max = 0;
    double*                fly_speed = nullptr;   // m/s
    double                 radius    = 0;         // set once the world loads
    double                 map_h     = 0;
    render::WorldRenderer* world     = nullptr;
    bool*                  running   = nullptr;
};

// Defines Camera, Fog and the top-level helpers (teleport, look, screenshot,
// quit, help, clear) in the console's VM.
void install_bindings(Console& console, DebugHost& host);

} // namespace debug
