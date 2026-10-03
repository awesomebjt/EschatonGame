#pragma once

#include <cmath>

namespace physics {

// Pseudoforces in the habitat's rotating frame, in local map axes: x around the
// circumference (the direction the floor moves), y along the axis, z up (toward
// the axis).
//
// With the floor moving toward +x the angular velocity is −ω ŷ, so
//   Coriolis    a = −2 ω⃗ × v = 2ω (v_z, 0, −v_x)
//   centrifugal a = −ω² r ẑ, r = R − alt
// Moving spinward (+x) presses you into the floor; rising drifts you spinward;
// motion along the axis is untouched. The second cylinder counter-rotates: pass
// spin = −1 there.
struct RotatingFrame
{
    double omega = 0;    // rad/s
    double radius = 0;   // floor radius, m
    double spin = 1;     // +1: floor moves toward +x

    static RotatingFrame for_radius(double radius, double g = 9.81)
    {
        return {std::sqrt(g / radius), radius, 1.0};
    }

    // Acceleration on a free body at `alt` metres off the floor moving at `v`.
    void acceleration(double alt, const float v[3], double out[3]) const
    {
        const double w = omega * spin;
        out[0] = 2.0 * w * v[2];
        out[1] = 0.0;
        out[2] = -2.0 * w * v[0] - omega * omega * (radius - alt);
    }
};

} // namespace physics
