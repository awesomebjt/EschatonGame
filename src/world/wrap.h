#pragma once

#include <cmath>

namespace world {

// The circumference is the one map constant needed before manifest.json loads.
// load_manifest() rejects a manifest that disagrees with it.
inline constexpr double kMapW = 25100.0;

// Shortest-arc difference between two map X coordinates.
inline double wrap_dx(double dx) { return dx - kMapW * std::round(dx / kMapW); }

// Map X folded into [0, kMapW).
inline double wrap_x(double x)
{
    const double m = std::fmod(x, kMapW);
    return m < 0 ? m + kMapW : m;
}

} // namespace world
