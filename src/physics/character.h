#pragma once

#include <vector>

#include "physics/collision_world.h"
#include "physics/rotating_frame.h"

namespace physics {

// Upright capsule: the segment runs from radius to height − radius above the feet.
struct CapsuleShape
{
    float radius = 0.3f;
    float height = 1.8f;
};

struct CharacterParams
{
    float step_height  = 0.35f;   // ledges climbed without jumping
    float snap         = 0.3f;    // how far the feet follow the ground down when walking
    float max_slope    = 0.7071f; // cos of the steepest walkable slope (45°)
    float walk_speed   = 4.0f;    // m/s
    float sprint_speed = 8.0f;
    float jump_speed   = 5.0f;    // ~1.3 m apex at 1 g
    float ground_accel = 40.0f;   // m/s² toward the wished velocity
    float air_accel    = 4.0f;
};

// Kinematic character state in map space. Position is the bottom of the capsule;
// velocity is in local metres per second along (spinward, axial, up).
struct CharacterBody
{
    double x = 0, y = 0, alt = 0;
    float  v[3] = {0, 0, 0};
    bool   grounded = false;
    float  ground_normal[3] = {0, 0, 1};
};

struct MoveIntent
{
    float wish_x = 0, wish_y = 0;   // desired horizontal direction, length <= 1
    bool  sprint = false;
    bool  jump   = false;
};

// One fixed step of a kinematic capsule: input acceleration, rotating-frame
// pseudoforces, then move-and-slide against the static world in three passes
// (step up, horizontal, down with ground snap). `scratch` is reused storage.
void step_character(CharacterBody& body, const MoveIntent& intent, const CapsuleShape& shape,
                    const CharacterParams& params, const RotatingFrame& frame,
                    const CollisionWorld& world, double map_h, double dt, std::vector<Tri>& scratch);

// Deepest overlap of a capsule standing at the origin with any of `tris`; 0 when
// clear. For tests and debugging.
float capsule_penetration(const std::vector<Tri>& tris, const CapsuleShape& shape);

} // namespace physics
