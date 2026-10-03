#include "debug/bindings.h"

#include <cmath>
#include <cstdio>

#include <bgfx/bgfx.h>
#include <bx/math.h>

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/string.h>

#include "debug/console.h"
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
    teleport(x, y, alt = cur)   move the camera
    look(yaw, pitch = cur)      orient the camera, degrees (yaw 0 = along +Y)
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

    console.eval_startup(k_ruby_helpers);
}

} // namespace debug
