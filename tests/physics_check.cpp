// Headless checks for the player capsule against the real exported world: no
// window, no GPU. Loads world/, builds collision, runs scripted scenarios through
// the same ECS player code the game uses, and prints PASS/FAIL per scenario.
//
//   ./build/physics_check

#include <chrono>
#include <cmath>
#include <numbers>
#include <cstdio>
#include <vector>

#include <entt/entt.hpp>

#include "game/player.h"
#include "physics/character.h"
#include "physics/collision_world.h"
#include "physics/rotating_frame.h"
#include "world/world_loader.h"
#include "world/wrap.h"

namespace {

int g_failures = 0;

void check(bool ok, const char* name, const char* detail)
{
    std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, detail);
    if (!ok) ++g_failures;
}

struct Sim
{
    entt::registry                 reg;
    entt::entity                   player = entt::null;
    const physics::CollisionWorld& world;
    physics::RotatingFrame         frame;
    double                         map_h;
    float                          worst_penetration = 0;
    double                         min_clearance     = 1e9;   // feet above the ground under them

    Sim(const physics::CollisionWorld& w, const world::Manifest& m)
        : world(w), frame(physics::RotatingFrame::for_radius(m.radius)), map_h(m.map_h)
    {
        player = game::spawn_player(reg, m.radius, 0, 0, 0);
    }

    game::PlayerView view() const { return game::player_view(reg, player, frame.radius); }

    void place(double x, double y, double alt) { game::place_player(reg, player, frame.radius, x, y, alt); }

    // Runs `secs` of 120 Hz steps, sampling penetration after each frame-sized slice.
    void run(double secs, float forward = 0, float right = 0, float yaw = 0, bool jump = false)
    {
        game::PlayerInput in;
        in.forward = forward;
        in.right   = right;
        in.yaw     = yaw;
        if (jump) game::queue_jump(reg, player);
        std::vector<physics::Tri> tris;
        for (double t = 0; t < secs - 1e-9; t += 1.0 / 60.0) {
            game::update_player(reg, player, in, world, frame, map_h, std::min(1.0 / 60.0, secs - t));
            const game::PlayerView v = view();
            tris.clear();
            world.gather(v.x, v.y, v.alt, 1.0f, tris);
            worst_penetration = std::max(worst_penetration, physics::capsule_penetration(tris, {}));
            if (auto g = world.raycast_down(v.x, v.y, v.alt + 0.5))
                min_clearance = std::min(min_clearance, v.alt - g->alt);
        }
    }
};

// A building instance's local point in map coordinates.
void building_to_map(const world::Manifest& m, const world::BuildingInstance& b, double lx, double ly,
                     double& x, double& y)
{
    const double c = std::cos(b.rot), s = std::sin(b.rot);
    x = world::wrap_x((b.col + 0.5) * m.chunk_x + b.x + c * lx - s * ly);
    y = (b.row + 0.5) * m.chunk_y + b.y + s * lx + c * ly;
}

void map_to_building(const world::Manifest& m, const world::BuildingInstance& b, double x, double y,
                     double& lx, double& ly)
{
    const double dx = world::wrap_dx(x - ((b.col + 0.5) * m.chunk_x + b.x));
    const double dy = y - ((b.row + 0.5) * m.chunk_y + b.y);
    const double c = std::cos(b.rot), s = std::sin(b.rot);
    lx = c * dx + s * dy;
    ly = -s * dx + c * dy;
}

} // namespace

int main()
{
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    auto w = world::load_world(WORLD_DIR);
    if (!w) {
        std::fprintf(stderr, "could not load %s\n", WORLD_DIR);
        return 2;
    }
    physics::CollisionWorld cw;
    const auto t1 = clock::now();
    if (!cw.build(*w)) return 2;
    const auto t2 = clock::now();
    const world::Manifest& m = w->manifest;
    std::printf("world %.2f s, collision build %.2f s\n",
                std::chrono::duration<double>(t1 - t0).count(), std::chrono::duration<double>(t2 - t1).count());

    char buf[256];

    // -- Spawn and settle --------------------------------------------------------
    {
        Sim sim(cw, m);
        double x = 0.5 * m.map_w, y = 0.5 * m.map_h, alt = 0;
        const bool found = game::find_spawn(cw, x, y, alt);
        sim.place(x, y, alt);
        sim.run(2.0);
        const auto v = sim.view();
        std::snprintf(buf, sizeof buf, "at (%.1f, %.1f) alt %.3f, %s", v.x, v.y, v.alt,
                      v.grounded ? "grounded" : "airborne");
        check(found && v.grounded && std::fabs(v.alt - alt) < 0.3, "spawn settles on the street", buf);

        // Wander: sprint in eight directions for 15 s each from the spawn.
        float worst = 0;
        double lowest = 1e9;
        for (int k = 0; k < 8; ++k) {
            sim.place(x, y, alt);
            sim.worst_penetration = 0;
            sim.min_clearance = 1e9;
            sim.run(15.0, 1.0f, 0.3f, static_cast<float>(k * std::numbers::pi / 4));
            worst  = std::max(worst, sim.worst_penetration);
            lowest = std::min(lowest, sim.min_clearance);
        }
        std::snprintf(buf, sizeof buf, "worst penetration %.3f m, lowest clearance %.3f m", worst, lowest);
        check(worst < 0.03f && lowest > -0.05, "8 x 15 s wander stays out of walls", buf);
    }

    // -- Buildings: a solid wall, and a courtyard passage --------------------------
    {
        // Prototype extents per type, for the depth of the courtyard.
        std::vector<float> depth(w->prototypes.size(), 0);
        for (size_t t = 0; t < w->prototypes.size(); ++t)
            for (const auto& v : w->prototypes[t].vertices) depth[t] = std::max(depth[t], v.pos[1]);

        auto pick = [&](bool courtyard) -> std::pair<size_t, const world::BuildingInstance*> {
            for (size_t t = 0; t < w->buildings.size(); ++t)
                if (m.building_types[t].courtyard == courtyard && !w->buildings[t].empty())
                    return {t, &w->buildings[t][w->buildings[t].size() / 2]};
            return {0, nullptr};
        };

        // Facing a solid building's front from 3 m out, walk into it for 3 s.
        if (auto [type, b] = pick(false); b) {
            Sim sim(cw, m);
            double x, y;
            building_to_map(m, *b, 0, -3, x, y);
            sim.place(x, y, 0.1);
            sim.run(0.5);
            sim.run(3.0, 1.0f, 0, -b->rot);
            const auto v = sim.view();
            double lx, ly;
            map_to_building(m, *b, v.x, v.y, lx, ly);
            std::snprintf(buf, sizeof buf, "%s: stopped at local y %.2f (wall at 0), penetration %.3f",
                          m.building_types[type].name.c_str(), ly, sim.worst_penetration);
            check(ly < -0.25 && ly > -0.5 && sim.worst_penetration < 0.03f, "wall stops the capsule", buf);
        }

        // Same toward a courtyard building's passage (2 m wide at x = 0).
        if (auto [type, b] = pick(true); b) {
            Sim sim(cw, m);
            double x, y;
            building_to_map(m, *b, 0, -3, x, y);
            sim.place(x, y, 0.1);
            sim.run(0.5);
            sim.run(5.0, 1.0f, 0, -b->rot);
            const auto v = sim.view();
            double lx, ly;
            map_to_building(m, *b, v.x, v.y, lx, ly);
            std::snprintf(buf, sizeof buf, "%s: reached local y %.1f (courtyard 10..%.0f), penetration %.3f",
                          m.building_types[type].name.c_str(), ly, depth[type] - 10,
                          sim.worst_penetration);
            check(ly > 10 && ly < depth[type] - 10 && sim.worst_penetration < 0.03f,
                  "walks through a courtyard passage", buf);
        }
    }

    // -- Jump and fall: height, and Coriolis drift --------------------------------
    {
        Sim sim(cw, m);
        double x = 0.5 * m.map_w, y = 0.5 * m.map_h, alt = 0;
        game::find_spawn(cw, x, y, alt);
        sim.place(x, y, alt);
        sim.run(1.0);
        const double base = sim.view().alt;
        game::queue_jump(sim.reg, sim.player);
        double apex = base;
        for (int k = 0; k < 120; ++k) {
            sim.run(1.0 / 120.0);
            apex = std::max(apex, sim.view().alt);
        }
        sim.run(1.0);
        std::snprintf(buf, sizeof buf, "apex %.2f m, %s after", apex - base,
                      sim.view().grounded ? "grounded" : "airborne");
        check(apex - base > 1.1 && apex - base < 1.5 && sim.view().grounded, "jump height ~1.3 m", buf);

        // Drop from 100 m. A falling body keeps the slower tangential speed of its
        // starting radius, so the floor overtakes it: it lands anti-spinward (−x).
        // For h << R the classic estimate is (2/3)·ω·sqrt(2h³/g).
        const double h = 100.0;
        sim.place(x, y, base + h);
        double t = 0;
        while (!sim.view().grounded && t < 20) {
            sim.run(1.0 / 120.0);
            t += 1.0 / 120.0;
        }
        const auto   v        = sim.view();
        const double drift    = world::wrap_dx(v.x - x);
        // It may drift onto a roof, so judge against the height actually fallen.
        const double fallen   = base + h - v.alt;
        const double estimate = -2.0 / 3.0 * sim.frame.omega * std::sqrt(2.0 * fallen * fallen * fallen / 9.81);
        std::snprintf(buf, sizeof buf, "fell %.1f s to alt %.1f, drift %+.2f m (estimate %+.2f), axial %+.3f m",
                      t, v.alt, drift, estimate, v.y - y);
        check(drift < 0.8 * estimate && drift > 1.2 * estimate && std::fabs(v.y - y) < 0.01,
              "100 m drop drifts anti-spinward", buf);
    }

    // -- Cost -------------------------------------------------------------------------
    {
        Sim sim(cw, m);
        double x = 0.5 * m.map_w, y = 0.5 * m.map_h, alt = 0;
        game::find_spawn(cw, x, y, alt);
        sim.place(x, y, alt);
        const auto s0 = clock::now();
        game::PlayerInput in;
        in.forward = 1;
        in.sprint  = true;
        for (int k = 0; k < 1200; ++k)
            game::update_player(sim.reg, sim.player, in, cw, sim.frame, sim.map_h, 1.0 / 120.0);
        const double us = std::chrono::duration<double, std::micro>(clock::now() - s0).count() / 1200;
        std::snprintf(buf, sizeof buf, "%.1f us per 120 Hz step", us);
        check(us < 500, "step cost", buf);
    }

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
