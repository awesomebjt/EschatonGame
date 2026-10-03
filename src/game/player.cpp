#include "game/player.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include <entt/entt.hpp>

#include "ecs/components/physics.h"
#include "ecs/components/player.h"
#include "ecs/components/spatial.h"
#include "physics/character.h"
#include "physics/collision_world.h"
#include "physics/rotating_frame.h"
#include "render/world_renderer.h"
#include "world/wrap.h"

namespace game {

namespace {

constexpr double k_step      = 1.0 / 120.0;
constexpr double k_max_frame = 0.25;   // after a hitch, drop time rather than spiral

ecs::CylindricalPosition to_cylindrical(double radius, double x, double y, double alt)
{
    return {radius - alt, world::wrap_x(x) / radius, y};
}

void to_map(const ecs::CylindricalPosition& p, double radius, double& x, double& y, double& alt)
{
    x   = p.theta * radius;
    y   = p.z;
    alt = radius - p.r;
}

} // namespace

entt::entity spawn_player(entt::registry& reg, double radius, double x, double y, double feet_alt)
{
    const entt::entity e = reg.create();
    reg.emplace<ecs::PlayerTag>(e);
    reg.emplace<ecs::PlayerControl>(e);
    reg.emplace<ecs::CylindricalPosition>(e, to_cylindrical(radius, x, y, feet_alt));
    reg.emplace<ecs::Velocity>(e);
    reg.emplace<ecs::CapsuleCollider>(e);
    reg.emplace<ecs::CharacterState>(e);
    reg.emplace<ecs::GravityAffected>(e);
    return e;
}

void place_player(entt::registry& reg, entt::entity e, double radius, double x, double y, double feet_alt)
{
    reg.get<ecs::CylindricalPosition>(e) = to_cylindrical(radius, x, y, feet_alt);
    reg.get<ecs::Velocity>(e)            = {};
    reg.get<ecs::CharacterState>(e)      = {};
}

std::optional<double> ground_at(const physics::CollisionWorld& world, double x, double y)
{
    if (auto hit = world.raycast_down(x, y, 1e4)) return hit->alt;
    return std::nullopt;
}

bool find_spawn(const physics::CollisionWorld& world, double& x, double& y, double& feet_alt, double search)
{
    // Street level only: roofs, roundabout platforms (1.5 m) and bridge decks are
    // all real surfaces, but not where you want to appear.
    constexpr double k_ring = 4.0;
    for (double r = 0; r <= search; r += k_ring) {
        const int n = std::max(1, static_cast<int>(std::ceil(2.0 * std::numbers::pi * r / k_ring)));
        for (int k = 0; k < n; ++k) {
            const double a  = 2.0 * std::numbers::pi * k / n;
            const double px = world::wrap_x(x + r * std::cos(a));
            const double py = y + r * std::sin(a);
            const auto   hit = world.raycast_down(px, py, 1e4);
            if (hit && !hit->building && hit->alt < 0.5) {
                x        = px;
                y        = py;
                feet_alt = hit->alt + 0.05;
                return true;
            }
        }
    }
    return false;
}

void queue_jump(entt::registry& reg, entt::entity e) { reg.get<ecs::PlayerControl>(e).jump_queued = true; }

void update_player(entt::registry& reg, entt::entity e, const PlayerInput& input,
                   const physics::CollisionWorld& world, const physics::RotatingFrame& frame,
                   double map_h, double dt)
{
    auto& control = reg.get<ecs::PlayerControl>(e);
    auto& pos     = reg.get<ecs::CylindricalPosition>(e);
    auto& vel     = reg.get<ecs::Velocity>(e);
    auto& state   = reg.get<ecs::CharacterState>(e);
    const auto& capsule = reg.get<ecs::CapsuleCollider>(e);

    // Look direction to a map-space wish: forward is (sin yaw, cos yaw) in (x, y),
    // right is (cos yaw, −sin yaw), matching the camera.
    physics::MoveIntent intent;
    const float sy = std::sin(input.yaw), cy = std::cos(input.yaw);
    intent.wish_x = input.forward * sy + input.right * cy;
    intent.wish_y = input.forward * cy - input.right * sy;
    const float len = std::hypot(intent.wish_x, intent.wish_y);
    if (len > 1.0f) {
        intent.wish_x /= len;
        intent.wish_y /= len;
    }
    intent.sprint = input.sprint;

    static const physics::CharacterParams k_params;
    static std::vector<physics::Tri>      scratch;   // reused across steps

    control.accumulator = std::min(control.accumulator + dt, k_max_frame);
    while (control.accumulator >= k_step) {
        control.accumulator -= k_step;
        // A press waits for the next step even if this frame was too short to have one.
        intent.jump = control.jump_queued;
        control.jump_queued = false;

        physics::CharacterBody body;
        to_map(pos, frame.radius, body.x, body.y, body.alt);
        body.v[0] = vel.x;
        body.v[1] = vel.y;
        body.v[2] = vel.z;
        body.grounded = state.grounded;
        std::copy(std::begin(state.ground_normal), std::end(state.ground_normal), body.ground_normal);

        physics::step_character(body, intent, {capsule.radius, capsule.height}, k_params, frame, world,
                                map_h, k_step, scratch);

        pos = to_cylindrical(frame.radius, body.x, body.y, body.alt);
        vel = {body.v[0], body.v[1], body.v[2]};
        state.grounded = body.grounded;
        std::copy(std::begin(body.ground_normal), std::end(body.ground_normal), state.ground_normal);
    }
}

PlayerView player_view(const entt::registry& reg, entt::entity e, double radius)
{
    PlayerView v;
    to_map(reg.get<ecs::CylindricalPosition>(e), radius, v.x, v.y, v.alt);
    const auto& vel = reg.get<ecs::Velocity>(e);
    v.v[0] = vel.x;
    v.v[1] = vel.y;
    v.v[2] = vel.z;
    v.grounded = reg.get<ecs::CharacterState>(e).grounded;
    return v;
}

render::CameraPos player_eye(const entt::registry& reg, entt::entity e, double radius)
{
    const PlayerView v = player_view(reg, e, radius);
    return {v.x, v.y, v.alt + k_eye_height};
}

} // namespace game
