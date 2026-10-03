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
#include "game/player.h"
#include "physics/collision_world.h"
#include "physics/rotating_frame.h"
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

mrb_value fog_reset(mrb_state*, mrb_value)
{
    g_host->world->fog = render::FogParams{};
    return mrb_nil_value();
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

module Fog
  def self.inspect
    format("#<Fog density=%g opaque=%.0f clear=%.0f>", density, opaque, clear)
  end
end

def teleport(x, y, alt = Camera.alt)
  Camera.x = x
  Camera.y = y
  Camera.alt = alt
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
    look(yaw, pitch = cur)      orient the camera, degrees (yaw 0 = along +Y)
    Player                      Player.pos .vel (map metres, feet), .grounded?, .noclip (settable; F key)
    Player.drop(x, y)           put the player on the nearest street near (x, y)
    Player.simulate(secs, forward: 1, right: 0, sprint: false, jump: false)   run physics now
    Fog                         haze; Fog.density .opaque .clear (settable), Fog.reset
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

    console.eval_startup(k_ruby_helpers);
}

} // namespace debug
