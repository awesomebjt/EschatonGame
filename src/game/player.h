#pragma once

#include <optional>

#include <entt/entity/fwd.hpp>

namespace physics { class CollisionWorld; struct RotatingFrame; }
namespace render { struct CameraPos; }

namespace game {

inline constexpr double k_eye_height = 1.65;   // m above the feet

struct PlayerInput
{
    float forward = 0, right = 0;   // -1..1
    float yaw     = 0;              // radians; 0 looks along +Y (map)
    bool  sprint  = false;
};

// Map-space snapshot of the player, feet position.
struct PlayerView
{
    double x = 0, y = 0, alt = 0;
    float  v[3] = {0, 0, 0};
    bool   grounded = false;
};

entt::entity spawn_player(entt::registry& reg, double radius, double x, double y, double feet_alt);

// Moves the player to (x, y, feet_alt) at rest.
void place_player(entt::registry& reg, entt::entity e, double radius, double x, double y, double feet_alt);

// Nearest street-level spot to (x, y): a terrain hit (not a roof, platform or
// monument) within `search` metres, searched in rings outward.
std::optional<double> ground_at(const physics::CollisionWorld& world, double x, double y);
bool find_spawn(const physics::CollisionWorld& world, double& x, double& y, double& feet_alt,
                double search = 400.0);

void queue_jump(entt::registry& reg, entt::entity e);

// PlayerMovementSystem + PhysicsSystem for the player: runs fixed 120 Hz steps
// of the capsule controller for the time elapsed.
void update_player(entt::registry& reg, entt::entity e, const PlayerInput& input,
                   const physics::CollisionWorld& world, const physics::RotatingFrame& frame,
                   double map_h, double dt);

PlayerView player_view(const entt::registry& reg, entt::entity e, double radius);

// Camera position at the player's eyes.
render::CameraPos player_eye(const entt::registry& reg, entt::entity e, double radius);

} // namespace game
