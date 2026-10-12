#include "debug/bindings.h"

#include <cmath>
#include <cstdio>

#include <bgfx/bgfx.h>
#include <bx/math.h>

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/string.h>

#include <entt/entt.hpp>

#include "debug/console.h"
#include "ecs/components/physics.h"
#include "game/player.h"
#include "net/client.h"
#include "physics/character.h"
#include "physics/collision_world.h"
#include "physics/rotating_frame.h"
#include "render/cloud_renderer.h"
#include "render/world_renderer.h"
#include "world/wrap.h"

namespace debug {

namespace {

// mruby C functions get no closure; the host is process-wide anyway.
DebugHost* g_host = nullptr;

// Nothing here may own a C++ object with a destructor across mrb_get_args or
// mrb_raise: mruby unwinds with longjmp.

mrb_float arg_float(mrb_state* mrb)
{
    mrb_float v;
    mrb_get_args(mrb, "f", &v);
    return v;
}

// -- Camera ------------------------------------------------------------------

mrb_value cam_x(mrb_state* mrb, mrb_value)   { return mrb_float_value(mrb, g_host->cam->x); }
mrb_value cam_y(mrb_state* mrb, mrb_value)   { return mrb_float_value(mrb, g_host->cam->y); }
mrb_value cam_alt(mrb_state* mrb, mrb_value) { return mrb_float_value(mrb, g_host->cam->alt); }
mrb_value cam_yaw(mrb_state* mrb, mrb_value) { return mrb_float_value(mrb, bx::toDeg(*g_host->yaw)); }
mrb_value cam_pitch(mrb_state* mrb, mrb_value)
{
    return mrb_float_value(mrb, bx::toDeg(*g_host->pitch));
}
mrb_value cam_speed(mrb_state* mrb, mrb_value) { return mrb_float_value(mrb, *g_host->fly_speed); }

mrb_value cam_set_x(mrb_state* mrb, mrb_value)
{
    const mrb_float v = arg_float(mrb);
    g_host->cam->x = world::wrap_x(v);
    return mrb_float_value(mrb, g_host->cam->x);
}

mrb_value cam_set_y(mrb_state* mrb, mrb_value)
{
    const mrb_float v = arg_float(mrb);
    if (g_host->map_h > 0 && (v < 0 || v > g_host->map_h))
        mrb_raisef(mrb, E_RANGE_ERROR, "y must be within 0..%f", g_host->map_h);
    g_host->cam->y = v;
    return mrb_float_value(mrb, v);
}

mrb_value cam_set_alt(mrb_state* mrb, mrb_value)
{
    const mrb_float v = arg_float(mrb);
    // Same bounds as free-fly: eye height off the floor, short of the light column.
    const double top = g_host->radius > 0 ? g_host->radius - 100.0 : 1e9;
    if (v < 1.5 || v > top) mrb_raisef(mrb, E_RANGE_ERROR, "alt must be within 1.5..%f", top);
    g_host->cam->alt = v;
    return mrb_float_value(mrb, v);
}

mrb_value cam_set_yaw(mrb_state* mrb, mrb_value)
{
    const mrb_float v = arg_float(mrb);
    *g_host->yaw = std::remainder(bx::toRad(float(v)), bx::kPi2);
    return mrb_float_value(mrb, bx::toDeg(*g_host->yaw));
}

mrb_value cam_set_pitch(mrb_state* mrb, mrb_value)
{
    const mrb_float v = arg_float(mrb);
    *g_host->pitch = bx::clamp(bx::toRad(float(v)), -g_host->pitch_max, g_host->pitch_max);
    return mrb_float_value(mrb, bx::toDeg(*g_host->pitch));
}

mrb_value cam_set_speed(mrb_state* mrb, mrb_value)
{
    const mrb_float v = arg_float(mrb);
    if (v <= 0) mrb_raise(mrb, E_ARGUMENT_ERROR, "speed must be positive");
    *g_host->fly_speed = v;
    return mrb_float_value(mrb, v);
}

// -- Player ------------------------------------------------------------------

bool player_ready()
{
    return g_host->registry && g_host->player && g_host->registry->valid(*g_host->player);
}

void require_player(mrb_state* mrb)
{
    if (!player_ready()) mrb_raise(mrb, E_RUNTIME_ERROR, "no player yet (world not loaded)");
}

mrb_value vec3_value(mrb_state* mrb, double a, double b, double c)
{
    const mrb_value v[3] = {mrb_float_value(mrb, a), mrb_float_value(mrb, b), mrb_float_value(mrb, c)};
    return mrb_ary_new_from_values(mrb, 3, v);
}

mrb_value player_pos(mrb_state* mrb, mrb_value)
{
    require_player(mrb);
    const game::PlayerView v = game::player_view(*g_host->registry, *g_host->player, g_host->radius);
    return vec3_value(mrb, v.x, v.y, v.alt);
}

mrb_value player_vel(mrb_state* mrb, mrb_value)
{
    require_player(mrb);
    const game::PlayerView v = game::player_view(*g_host->registry, *g_host->player, g_host->radius);
    return vec3_value(mrb, v.v[0], v.v[1], v.v[2]);
}

mrb_value player_grounded(mrb_state* mrb, mrb_value)
{
    require_player(mrb);
    return mrb_bool_value(game::player_view(*g_host->registry, *g_host->player, g_host->radius).grounded);
}

mrb_value player_noclip(mrb_state*, mrb_value) { return mrb_bool_value(g_host->noclip && *g_host->noclip); }

mrb_value player_set_noclip(mrb_state* mrb, mrb_value)
{
    mrb_bool on;
    mrb_get_args(mrb, "b", &on);
    require_player(mrb);
    // Same as the F key: leaving noclip drops the player in at the camera.
    if (*g_host->noclip && !on) {
        const render::CameraPos& c = *g_host->cam;
        game::place_player(*g_host->registry, *g_host->player, g_host->radius, c.x, c.y,
                           c.alt - game::k_eye_height);
    }
    *g_host->noclip = on;
    return mrb_bool_value(on);
}

// Player.drop(x, y): nearest street-level spot, at rest.
mrb_value player_drop(mrb_state* mrb, mrb_value)
{
    mrb_float x, y;
    mrb_get_args(mrb, "ff", &x, &y);
    require_player(mrb);
    double px = x, py = y, alt = 0;
    if (!game::find_spawn(*g_host->collision, px, py, alt))
        mrb_raise(mrb, E_RUNTIME_ERROR, "no street-level ground within 400 m");
    game::place_player(*g_host->registry, *g_host->player, g_host->radius, px, py, alt);
    *g_host->noclip = false;
    // The camera must agree, or the walk loop reads it as a console teleport.
    *g_host->cam = game::player_eye(*g_host->registry, *g_host->player, g_host->radius);
    return vec3_value(mrb, px, py, alt);
}

// Player.__simulate(seconds, forward, right, sprint, jump): advance the player's
// physics right now, with the current yaw. Headless testing of movement.
mrb_value player_simulate(mrb_state* mrb, mrb_value)
{
    mrb_float secs, fwd, right;
    mrb_bool  sprint, jump;
    mrb_get_args(mrb, "fffbb", &secs, &fwd, &right, &sprint, &jump);
    require_player(mrb);
    if (secs < 0 || secs > 600) mrb_raise(mrb, E_ARGUMENT_ERROR, "seconds must be within 0..600");
    game::PlayerInput in;
    in.forward = static_cast<float>(fwd);
    in.right   = static_cast<float>(right);
    in.yaw     = *g_host->yaw;
    in.sprint  = sprint;
    if (jump) game::queue_jump(*g_host->registry, *g_host->player);
    // update_player caps a single call's catch-up, so feed it slices.
    for (double left = secs; left > 0; left -= 0.1)
        game::update_player(*g_host->registry, *g_host->player, in, *g_host->collision, *g_host->frame,
                            g_host->map_h, std::min(left, 0.1));
    return mrb_nil_value();
}

// -- Fog ---------------------------------------------------------------------

mrb_value fog_density(mrb_state* mrb, mrb_value) { return mrb_float_value(mrb, g_host->world->fog.density); }
mrb_value fog_opaque(mrb_state* mrb, mrb_value)  { return mrb_float_value(mrb, g_host->world->fog.opaque); }
mrb_value fog_clear(mrb_state* mrb, mrb_value)   { return mrb_float_value(mrb, g_host->world->fog.clear); }

mrb_value fog_set_density(mrb_state* mrb, mrb_value)
{
    const mrb_float v = arg_float(mrb);
    if (v <= 0) mrb_raise(mrb, E_ARGUMENT_ERROR, "density must be positive");
    g_host->world->fog.density = float(v);
    return mrb_float_value(mrb, v);
}

// The shader divides by the exponential over (opaque - clear), so keep a gap.
mrb_value fog_set_opaque(mrb_state* mrb, mrb_value)
{
    const mrb_float v = arg_float(mrb);
    if (v <= g_host->world->fog.clear) mrb_raise(mrb, E_ARGUMENT_ERROR, "opaque must be beyond Fog.clear");
    g_host->world->fog.opaque = float(v);
    return mrb_float_value(mrb, v);
}

mrb_value fog_set_clear(mrb_state* mrb, mrb_value)
{
    const mrb_float v = arg_float(mrb);
    if (v < 0 || v >= g_host->world->fog.opaque)
        mrb_raise(mrb, E_ARGUMENT_ERROR, "clear must be within 0...Fog.opaque");
    g_host->world->fog.clear = float(v);
    return mrb_float_value(mrb, v);
}

mrb_value fog_color(mrb_state* mrb, mrb_value)
{
    char hex[8];
    std::snprintf(hex, sizeof hex, "#%06x", unsigned(g_host->world->fog.rgba >> 8));
    return mrb_str_new_cstr(mrb, hex);
}

// Takes "#rgb", "#rrggbb" (the # optional) or an Integer 0xRRGGBB.
mrb_value fog_set_color(mrb_state* mrb, mrb_value)
{
    mrb_value v;
    mrb_get_args(mrb, "o", &v);
    uint32_t rgb = 0;
    if (mrb_integer_p(v)) {
        const mrb_int n = mrb_integer(v);
        if (n < 0 || n > 0xffffff) mrb_raise(mrb, E_ARGUMENT_ERROR, "colour must be within 0..0xFFFFFF");
        rgb = uint32_t(n);
    } else if (mrb_string_p(v)) {
        const char* p   = RSTRING_PTR(v);
        mrb_int     len = RSTRING_LEN(v);
        if (len > 0 && p[0] == '#') ++p, --len;
        if (len != 3 && len != 6) mrb_raise(mrb, E_ARGUMENT_ERROR, "expected \"#rgb\" or \"#rrggbb\"");
        for (mrb_int i = 0; i < len; ++i) {
            const char c = p[i];
            const int  d = c >= '0' && c <= '9' ? c - '0'
                         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) mrb_raise(mrb, E_ARGUMENT_ERROR, "not a hex colour");
            rgb = len == 3 ? (rgb << 8) | uint32_t(d * 17) : (rgb << 4) | uint32_t(d);
        }
    } else {
        mrb_raise(mrb, E_TYPE_ERROR, "expected a hex String or an Integer");
    }
    g_host->world->fog.rgba = (rgb << 8) | 0xff;
    return v;
}

mrb_value fog_reset(mrb_state*, mrb_value)
{
    g_host->world->fog = render::FogParams{};
    return mrb_nil_value();
}

// -- Clouds --------------------------------------------------------------------
// Shape parameters regenerate the field (~0.7 s); wind and shadow apply live.

#define CLOUD_PARAM(name, field, check, msg)                                         \
    mrb_value clouds_##name(mrb_state* mrb, mrb_value)                               \
    {                                                                                \
        return mrb_float_value(mrb, g_host->clouds->params().field);                 \
    }                                                                                \
    mrb_value clouds_set_##name(mrb_state* mrb, mrb_value)                           \
    {                                                                                \
        const mrb_float v = arg_float(mrb);                                          \
        if (!(check)) mrb_raise(mrb, E_ARGUMENT_ERROR, msg);                         \
        world::CloudParams p = g_host->clouds->params();                             \
        p.field = static_cast<float>(v);                                             \
        g_host->clouds->set_params(p);                                               \
        return mrb_float_value(mrb, v);                                              \
    }

CLOUD_PARAM(coverage, coverage, v >= 0 && v <= 0.6, "coverage must be within 0..0.6")
CLOUD_PARAM(alt, base_alt, v >= 200 && v <= 3000, "alt must be within 200..3000")
CLOUD_PARAM(lean, lean, v >= -1 && v <= 1, "lean must be within -1..1")
#undef CLOUD_PARAM

mrb_value clouds_seed(mrb_state* mrb, mrb_value)
{
    return mrb_int_value(mrb, static_cast<mrb_int>(g_host->clouds->params().seed));
}

mrb_value clouds_set_seed(mrb_state* mrb, mrb_value)
{
    mrb_int v;
    mrb_get_args(mrb, "i", &v);
    world::CloudParams p = g_host->clouds->params();
    p.seed = static_cast<uint32_t>(v);
    g_host->clouds->set_params(p);
    return mrb_int_value(mrb, v);
}

mrb_value clouds_wind(mrb_state* mrb, mrb_value)
{
    const world::CloudParams& p = g_host->clouds->params();
    const mrb_value v[2] = {mrb_float_value(mrb, p.wind_x), mrb_float_value(mrb, p.wind_y)};
    return mrb_ary_new_from_values(mrb, 2, v);
}

// Clouds.set_wind(spinward, axial), m/s. Doesn't regenerate.
mrb_value clouds_set_wind(mrb_state* mrb, mrb_value)
{
    mrb_float wx, wy;
    mrb_get_args(mrb, "ff", &wx, &wy);
    g_host->clouds->set_wind(static_cast<float>(wx), static_cast<float>(wy));
    return clouds_wind(mrb, mrb_nil_value());
}

mrb_value clouds_shadow(mrb_state* mrb, mrb_value) { return mrb_float_value(mrb, g_host->world->cloud_shadow.strength); }

mrb_value clouds_set_shadow(mrb_state* mrb, mrb_value)
{
    const mrb_float v = arg_float(mrb);
    if (v < 0 || v > 4) mrb_raise(mrb, E_ARGUMENT_ERROR, "shadow must be within 0..4");
    g_host->world->cloud_shadow.strength = static_cast<float>(v);
    return mrb_float_value(mrb, v);
}

mrb_value clouds_enabled(mrb_state*, mrb_value) { return mrb_bool_value(g_host->clouds->enabled); }

mrb_value clouds_set_enabled(mrb_state* mrb, mrb_value)
{
    mrb_bool on;
    mrb_get_args(mrb, "b", &on);
    g_host->clouds->enabled = on;
    return mrb_bool_value(on);
}

mrb_value clouds_count(mrb_state* mrb, mrb_value)
{
    const mrb_value v[3] = {mrb_int_value(mrb, static_cast<mrb_int>(g_host->clouds->cloud_count())),
                            mrb_int_value(mrb, static_cast<mrb_int>(g_host->clouds->puff_count())),
                            mrb_int_value(mrb, static_cast<mrb_int>(g_host->clouds->drawn_puffs()))};
    return mrb_ary_new_from_values(mrb, 3, v);
}

// -- Multiplayer ---------------------------------------------------------------
// Players are referred to by colour index (net::PlayerColor order); the Ruby side
// wraps them in PlayerRef objects.

mrb_value net_status(mrb_state* mrb, mrb_value) { return mrb_str_new_cstr(mrb, g_host->net->describe().c_str()); }

mrb_value net_me_color(mrb_state* mrb, mrb_value)
{
    if (!g_host->net->in_session()) return mrb_nil_value();
    return mrb_int_value(mrb, static_cast<mrb_int>(*g_host->net->color()));
}

net::PlayerColor arg_color(mrb_state* mrb)
{
    mrb_int c;
    mrb_get_args(mrb, "i", &c);
    if (c < 0 || c >= net::k_max_players) mrb_raise(mrb, E_ARGUMENT_ERROR, "no such player colour");
    return static_cast<net::PlayerColor>(c);
}

// __net_state(color) → [x, y, alt, yaw, pitch, vx, vy, vz, grounded, noclip] or nil
// (degrees for the angles), as this client currently draws that player.
mrb_value net_state(mrb_state* mrb, mrb_value)
{
    const net::PlayerColor c = arg_color(mrb);
    const auto st = g_host->net->remote(c, *g_host->net_time);
    if (!st) return mrb_nil_value();
    const mrb_value v[10] = {mrb_float_value(mrb, st->x), mrb_float_value(mrb, st->y),
                             mrb_float_value(mrb, st->alt), mrb_float_value(mrb, bx::toDeg(st->yaw)),
                             mrb_float_value(mrb, bx::toDeg(st->pitch)), mrb_float_value(mrb, st->vel[0]),
                             mrb_float_value(mrb, st->vel[1]), mrb_float_value(mrb, st->vel[2]),
                             mrb_bool_value(st->flags & net::k_flag_grounded),
                             mrb_bool_value(st->flags & net::k_flag_noclip)};
    return mrb_ary_new_from_values(mrb, 10, v);
}

// __teleport_near(color): put this player next to another one, facing them.
// Tries a step behind them, then to either side, then in front, keeping only
// spots on the same floor with room for the capsule; failing all of those,
// their own spot (players don't collide with each other). A player flying in
// noclip is joined in noclip.
mrb_value net_teleport_near(mrb_state* mrb, mrb_value)
{
    const net::PlayerColor c = arg_color(mrb);
    require_player(mrb);
    const auto found = g_host->net->remote(c, *g_host->net_time);
    if (!found) mrb_raise(mrb, E_RUNTIME_ERROR, "that player is not in the game");
    const net::PlayerState t = *found;
    const double R = g_host->radius;

    if (t.flags & net::k_flag_noclip) {
        *g_host->noclip = true;
        *g_host->cam    = {world::wrap_x(t.x - 2.0 * std::sin(t.yaw)), t.y - 2.0 * std::cos(t.yaw),
                           t.alt + game::k_eye_height};
        *g_host->yaw    = t.yaw;
        return vec3_value(mrb, g_host->cam->x, g_host->cam->y, g_host->cam->alt - game::k_eye_height);
    }

    // No mrb_raise past this point: the scratch vector has a destructor.
    constexpr double k_gap = 1.5;   // m between capsule centres
    const double fx = std::sin(t.yaw), fy = std::cos(t.yaw);
    const double offsets[4][2] = {{-fx, -fy}, {fy, -fx}, {-fy, fx}, {fx, fy}};   // behind, right, left, front
    const ecs::CapsuleCollider cap;
    const physics::CapsuleShape shape{cap.radius, cap.height};
    std::vector<physics::Tri> tris;

    double px = t.x, py = t.y, palt = t.alt;
    for (const auto& o : offsets) {
        const double cx = world::wrap_x(t.x + k_gap * o[0]);
        const double cy = t.y + k_gap * o[1];
        const auto   hit = g_host->collision->raycast_down(cx, cy, t.alt + 1.0);
        if (!hit || std::fabs(hit->alt - t.alt) > 0.6) continue;   // a wall, a drop or a ledge
        tris.clear();
        g_host->collision->gather(cx, cy, hit->alt + 0.02, 1.5f, tris);
        if (physics::capsule_penetration(tris, shape) > 1e-3f) continue;
        px = cx, py = cy, palt = hit->alt + 0.02;
        break;
    }
    game::place_player(*g_host->registry, *g_host->player, R, px, py, palt);
    *g_host->noclip = false;
    *g_host->cam    = game::player_eye(*g_host->registry, *g_host->player, R);
    // Face them; when standing on their spot, look where they look.
    const double dx = world::wrap_dx(t.x - px), dy = t.y - py;
    *g_host->yaw = dx * dx + dy * dy > 0.01 ? static_cast<float>(std::atan2(dx, dy)) : t.yaw;
    return vec3_value(mrb, px, py, palt);
}

// -- Misc --------------------------------------------------------------------

mrb_value screenshot(mrb_state* mrb, mrb_value)
{
    const char* path;
    mrb_get_args(mrb, "z", &path);
    // bgfx copies the path; the PNG is written by the callback a frame or two later.
    bgfx::requestScreenShot(BGFX_INVALID_HANDLE, path);
    return mrb_str_new_cstr(mrb, path);
}

mrb_value quit(mrb_state*, mrb_value)
{
    *g_host->running = false;
    return mrb_nil_value();
}

mrb_value clear(mrb_state* mrb, mrb_value)
{
    static_cast<Console*>(mrb->ud)->clear();
    return mrb_nil_value();
}

void define_accessor(mrb_state* mrb, RClass* mod, const char* name, mrb_func_t get, mrb_func_t set)
{
    char setter[32];
    snprintf(setter, sizeof(setter), "%s=", name);
    mrb_define_module_function(mrb, mod, name, get, MRB_ARGS_NONE());
    mrb_define_module_function(mrb, mod, setter, set, MRB_ARGS_REQ(1));
}

// Helpers that are easier to write in Ruby than against the C API.
constexpr const char* k_ruby_helpers = R"RUBY(
module Camera
  def self.pos
    [x, y, alt]
  end

  def self.inspect
    format("#<Camera x=%.1f y=%.1f alt=%.1f yaw=%.1f pitch=%.1f speed=%.1f>",
           x, y, alt, yaw, pitch, speed)
  end
end

module Player
  def self.simulate(seconds, forward: 0, right: 0, sprint: false, jump: false)
    __simulate(seconds, forward, right, sprint, jump)
    self
  end

  def self.inspect
    x, y, alt = pos
    vx, vy, vz = vel
    format("#<Player x=%.2f y=%.2f alt=%.2f v=(%.2f, %.2f, %.2f) %s%s>",
           x, y, alt, vx, vy, vz, grounded? ? "grounded" : "airborne", noclip ? " noclip" : "")
  end
end

module Clouds
  def self.inspect
    clouds, puffs, drawn = counts
    wx, wy = wind
    format("#<Clouds %d clouds, %d puffs (%d drawn) coverage=%.2f alt=%.0f lean=%.2f " \
           "wind=(%.1f, %.1f) shadow=%.2f seed=%d%s>",
           clouds, puffs, drawn, coverage, alt, lean, wx, wy, shadow, seed, enabled ? "" : " off")
  end
end

module Fog
  def self.inspect
    format("#<Fog density=%g opaque=%.0f clear=%.0f color=%s>", density, opaque, clear, color)
  end
end

# A player in a multiplayer session, by colour, or this player (me). Reading a
# position raises if that player isn't in the game.
class PlayerRef
  COLORS = %w[BLUE GREEN RED YELLOW]   # net::PlayerColor order

  attr_reader :index   # colour index, nil for me

  def initialize(index) @index = index end
  def self.me() @me ||= new(nil) end

  def me?() @index.nil? || @index == __net_me_color end
  def color
    i = @index || __net_me_color
    i && COLORS[i]
  end
  def online?() @index.nil? || me? || !__net_state(@index).nil? end

  def state
    s = __net_state(@index)
    raise "#{color} is not in the game" unless s
    s
  end
  def pos() me? ? Player.pos : state[0, 3] end
  def vel() me? ? Player.vel : state[5, 3] end
  def yaw() me? ? Camera.yaw : state[3] end

  def inspect
    return format("#<me %s>", color || "(offline)") + " " + Player.inspect if me?
    s = __net_state(@index)
    return "#<#{color} not in the game>" unless s
    format("#<%s x=%.2f y=%.2f alt=%.2f yaw=%.0f v=(%.2f, %.2f, %.2f) %s%s>",
           color, s[0], s[1], s[2], s[3], s[5], s[6], s[7], s[8] ? "grounded" : "airborne", s[9] ? " noclip" : "")
  end
  def to_s() color || "me" end
end

# `teleport me Players.RED` parses as teleport(me(Players.RED)): me with a
# target names the move.
class Teleport
  attr_reader :who, :target
  def initialize(who, target) @who, @target = who, target end
end

def me(target = nil)
  target ? Teleport.new(PlayerRef.me, target) : PlayerRef.me
end

module Players
  def self.BLUE()   PlayerRef.new(0) end
  def self.GREEN()  PlayerRef.new(1) end
  def self.RED()    PlayerRef.new(2) end
  def self.YELLOW() PlayerRef.new(3) end
  def self.all() (0...4).map { |i| PlayerRef.new(i) }.select(&:online?) end
  def self.each(&b) all.each(&b) end
  def self.status() __net_status end
  def self.inspect
    (["#<Players #{status}>"] + all.map(&:inspect)).join("\n  ")
  end
end

def teleport(*args)
  args = [args[0].who, args[0].target] if args.size == 1 && args[0].is_a?(Teleport)
  if args.any? { |a| a.is_a?(PlayerRef) }
    who, target = args.size == 1 ? [me, args[0]] : args
    unless args.size <= 2 && who.is_a?(PlayerRef) && target.is_a?(PlayerRef)
      raise ArgumentError, "usage: teleport(Players.RED) or teleport(me, Players.RED)"
    end
    raise ArgumentError, "you can only teleport yourself (me)" unless who.me?
    raise ArgumentError, "that's you" if target.me?
    raise ArgumentError, "#{target.color} is not in the game" unless target.online?
    __teleport_near(target.index)
    return me
  end
  raise ArgumentError, "usage: teleport(x, y, alt = Camera.alt)" unless args.size == 2 || args.size == 3
  x, y, alt = args
  Camera.x = x
  Camera.y = y
  Camera.alt = alt || Camera.alt
  Camera
end

def look(yaw, pitch = Camera.pitch)
  Camera.yaw = yaw
  Camera.pitch = pitch
  Camera
end

def help
  puts <<~TEXT
    Camera                      position/orientation; Camera.x .y .alt .yaw .pitch .speed (all settable)
    Camera.pos                  [x, y, alt] in map metres (x wraps at the circumference)
    teleport(x, y, alt = cur)   move the camera (the player too, falling from there, unless noclip)
    teleport me, Players.RED    snap next to another player (also: teleport Players.RED)
    Players                     the session: Players.BLUE .GREEN .RED .YELLOW, .all, .status
    me                          you: me.color .pos .vel .yaw; Players.RED.pos etc. for others
    look(yaw, pitch = cur)      orient the camera, degrees (yaw 0 = along +Y)
    Player                      Player.pos .vel (map metres, feet), .grounded?, .noclip (settable; F key)
    Player.drop(x, y)           put the player on the nearest street near (x, y)
    Player.simulate(secs, forward: 1, right: 0, sprint: false, jump: false)   run physics now
    Fog                         haze; Fog.density .opaque .clear .color (settable), Fog.reset
    Fog.color = "#c8a0ff"       haze colour as hex ("#rgb", "#rrggbb" or 0xRRGGBB); also the sky
    Clouds                      .coverage .alt .lean .seed (regenerate), .shadow .enabled, .set_wind(x, y)
    screenshot("file.png")      save the next frame (console included)
    clear                       clear this console      quit    exit the game
    Keys: Enter run, Up/Down history, PgUp/PgDn or wheel scroll, Ctrl+C cancel block,
          Ctrl+L clear, Ctrl+V paste, Esc or Shift+` close
  TEXT
end
)RUBY";

} // namespace

void install_bindings(Console& console, DebugHost& host)
{
    g_host = &host;
    mrb_state* mrb = console.vm();

    RClass* cam = mrb_define_module(mrb, "Camera");
    define_accessor(mrb, cam, "x", cam_x, cam_set_x);
    define_accessor(mrb, cam, "y", cam_y, cam_set_y);
    define_accessor(mrb, cam, "alt", cam_alt, cam_set_alt);
    define_accessor(mrb, cam, "yaw", cam_yaw, cam_set_yaw);
    define_accessor(mrb, cam, "pitch", cam_pitch, cam_set_pitch);
    define_accessor(mrb, cam, "speed", cam_speed, cam_set_speed);

    RClass* fog = mrb_define_module(mrb, "Fog");
    define_accessor(mrb, fog, "density", fog_density, fog_set_density);
    define_accessor(mrb, fog, "opaque", fog_opaque, fog_set_opaque);
    define_accessor(mrb, fog, "clear", fog_clear, fog_set_clear);
    define_accessor(mrb, fog, "color", fog_color, fog_set_color);
    mrb_define_module_function(mrb, fog, "reset", fog_reset, MRB_ARGS_NONE());

    mrb_define_method(mrb, mrb->kernel_module, "screenshot", screenshot, MRB_ARGS_REQ(1));
    mrb_define_method(mrb, mrb->kernel_module, "quit", quit, MRB_ARGS_NONE());
    mrb_define_method(mrb, mrb->kernel_module, "clear", clear, MRB_ARGS_NONE());

    RClass* player = mrb_define_module(mrb, "Player");
    mrb_define_module_function(mrb, player, "pos", player_pos, MRB_ARGS_NONE());
    mrb_define_module_function(mrb, player, "vel", player_vel, MRB_ARGS_NONE());
    mrb_define_module_function(mrb, player, "grounded?", player_grounded, MRB_ARGS_NONE());
    define_accessor(mrb, player, "noclip", player_noclip, player_set_noclip);
    mrb_define_module_function(mrb, player, "drop", player_drop, MRB_ARGS_REQ(2));
    mrb_define_module_function(mrb, player, "__simulate", player_simulate, MRB_ARGS_REQ(5));

    RClass* clouds = mrb_define_module(mrb, "Clouds");
    define_accessor(mrb, clouds, "coverage", clouds_coverage, clouds_set_coverage);
    define_accessor(mrb, clouds, "alt", clouds_alt, clouds_set_alt);
    define_accessor(mrb, clouds, "lean", clouds_lean, clouds_set_lean);
    define_accessor(mrb, clouds, "seed", clouds_seed, clouds_set_seed);
    define_accessor(mrb, clouds, "shadow", clouds_shadow, clouds_set_shadow);
    define_accessor(mrb, clouds, "enabled", clouds_enabled, clouds_set_enabled);
    mrb_define_module_function(mrb, clouds, "wind", clouds_wind, MRB_ARGS_NONE());
    mrb_define_module_function(mrb, clouds, "set_wind", clouds_set_wind, MRB_ARGS_REQ(2));
    mrb_define_module_function(mrb, clouds, "counts", clouds_count, MRB_ARGS_NONE());

    mrb_define_method(mrb, mrb->kernel_module, "__net_status", net_status, MRB_ARGS_NONE());
    mrb_define_method(mrb, mrb->kernel_module, "__net_me_color", net_me_color, MRB_ARGS_NONE());
    mrb_define_method(mrb, mrb->kernel_module, "__net_state", net_state, MRB_ARGS_REQ(1));
    mrb_define_method(mrb, mrb->kernel_module, "__teleport_near", net_teleport_near, MRB_ARGS_REQ(1));

    console.eval_startup(k_ruby_helpers);
}

} // namespace debug
