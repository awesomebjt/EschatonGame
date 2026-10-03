#pragma once

namespace ecs {

// Upright capsule; the bottom sits at the entity's position.
struct CapsuleCollider
{
    float radius = 0.3f;
    float height = 1.8f;
};

struct CharacterState
{
    bool  grounded         = false;
    float ground_normal[3] = {0, 0, 1};
};

struct GravityAffected {};   // tag: subject to centrifugal/Coriolis

} // namespace ecs
