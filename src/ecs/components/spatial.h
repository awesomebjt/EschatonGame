#pragma once

namespace ecs {

// Cylindrical world position in double precision (root CLAUDE.md, "Coordinate
// Systems"): theta = map x / R, z = map y, r = R − map altitude.
struct CylindricalPosition
{
    double r = 0, theta = 0, z = 0;
};

// Velocity in the rotating frame, local metres per second along (spinward,
// axial, up toward the axis).
struct Velocity
{
    float x = 0, y = 0, z = 0;
};

} // namespace ecs
