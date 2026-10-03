#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

#include <bgfx/bgfx.h>
#include <bimg/bimg.h>
#include <bx/file.h>
#include <bx/math.h>
#include <SDL3/SDL.h>
#include <entt/entt.hpp>

#include "debug/bindings.h"
#include "debug/console.h"
#include "game/player.h"
#include "physics/collision_world.h"
#include "physics/rotating_frame.h"
#include "render/cloud_renderer.h"
#include "render/world_renderer.h"
#include "world/world_loader.h"
#include "world/wrap.h"

static constexpr uint32_t k_width  = 1280;
static constexpr uint32_t k_height =  720;

// bgfx's embedded debug font is 8 px wide × 16 px tall.
static constexpr uint16_t k_cols = k_width  / 8;   // 160
static constexpr uint16_t k_rows = k_height / 16;  // 45

// Return the column that left-aligns a string of `len` chars at the centre.
static uint16_t cx(uint16_t len) { return (k_cols - len) / 2; }

enum class Screen { Title, Menu, Game };

// Scene state — created once on first Game entry, destroyed on shutdown.
static render::WorldRenderer g_world;
static render::CloudRenderer g_clouds;
static bool                  g_scene_loaded = false;
static bool                  g_load_failed  = false;
static int                   g_load_wait    = 0;   // frames the LOADING card has been up

// Camera position in map space. Walking, it sits at the player's eyes; in noclip
// it flies free. --pos sets it (the player then falls from there).
static render::CameraPos g_cam;
static bool              g_cam_from_args = false;

// The player: a capsule colliding with the static world in the rotating frame.
static entt::registry          g_registry;
static entt::entity            g_player = entt::null;
static physics::CollisionWorld g_collision;
static physics::RotatingFrame  g_frame;
static bool                    g_noclip = false;     // F toggles free-fly
static render::CameraPos       g_cam_written;        // camera as the player last set it

// Camera orientation controlled by mouselook, relative to the local "up" (toward
// the axis). Yaw 0 looks along the axis (+Y in map space); yaw wraps freely.
// Pitch tilts up/down, clamped to straight down / straight up.
static float g_cam_yaw              = 0.0f;
static float g_cam_pitch            = 0.0f;
static constexpr float k_mouse_sens = 0.002f;          // radians per pixel
static constexpr float k_pitch_max  = bx::kPiHalf;

static constexpr double k_start_altitude = 200.0;      // noclip fallback if no street is found
static double           g_fly_speed       = 40.0;      // m/s; Shift ×10, console-settable
static double           g_cylinder_radius = 0.0;       // from the manifest
static double           g_map_h           = 0.0;

// Drop-down Ruby console (Shift+`) and the game state its commands can reach.
static debug::Console   g_console;
static debug::DebugHost g_debug_host;
struct StartupLine { const char* code; bool show; };
static std::vector<StartupLine> g_console_lines;   // --console / --exec, run once the world loads

// --shot: render one frame of the world to a PNG and exit (for headless checks).
static const char* g_shot_path = nullptr;

// -----------------------------------------------------------------------
// bgfx callback: forwards traces and writes screenshots as PNG
// -----------------------------------------------------------------------

struct BgfxCallback final : public bgfx::CallbackI
{
    bool shot_done = false;

    void fatal(const char* file, uint16_t line, bgfx::Fatal::Enum code, const char* str) override
    {
        fprintf(stderr, "bgfx fatal %s:%u: %s\n", file, line, str);
        if (code != bgfx::Fatal::DebugCheck) abort();
    }
    void traceVargs(const char* /*file*/, uint16_t /*line*/, const char* format, va_list args) override
    {
        vfprintf(stderr, format, args);
    }
    void profilerBegin(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerBeginLiteral(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerEnd() override {}
    uint32_t cacheReadSize(uint64_t) override { return 0; }
    bool cacheRead(uint64_t, void*, uint32_t) override { return false; }
    void cacheWrite(uint64_t, const void*, uint32_t) override {}
    void screenShot(const char* path, uint32_t width, uint32_t height, uint32_t pitch,
                    bgfx::TextureFormat::Enum format, const void* data, uint32_t /*size*/,
                    bool yflip) override
    {
        bx::FileWriter writer;
        bx::Error      err;
        if (bx::open(&writer, bx::FilePath(path), false, &err)) {
            bimg::imageWritePng(&writer, width, height, pitch, data,
                                static_cast<bimg::TextureFormat::Enum>(format), yflip, &err);
            bx::close(&writer);
        }
        if (err.isOk()) printf("screenshot: %s (%ux%u)\n", path, width, height);
        else            fprintf(stderr, "screenshot: failed to write %s\n", path);
        shot_done = true;
    }
    void captureBegin(uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, bool) override {}
    void captureEnd() override {}
    void captureFrame(const void*, uint32_t) override {}
};

static BgfxCallback g_bgfx_callback;

// -----------------------------------------------------------------------
// Scene setup
// -----------------------------------------------------------------------

// Loads every chunk of the cylinder and uploads it. Blocking: a few seconds.
static bool load_scene()
{
    const uint64_t t0 = SDL_GetTicksNS();
    auto world = world::load_world(WORLD_DIR);
    if (!world) {
        fprintf(stderr, "load_scene: failed to load world from %s\n", WORLD_DIR);
        return false;
    }
    const uint64_t t1 = SDL_GetTicksNS();

    if (!g_world.create(*world)) {
        fprintf(stderr, "load_scene: failed to create world GPU resources\n");
        return false;
    }

    const world::Manifest& m = world->manifest;
    g_cylinder_radius = m.radius;
    g_map_h           = m.map_h;
    g_debug_host.radius = m.radius;
    g_debug_host.map_h  = m.map_h;

    const uint64_t t2 = SDL_GetTicksNS();
    if (!g_collision.build(*world)) {
        fprintf(stderr, "load_scene: failed to build collision\n");
        return false;
    }
    g_frame = physics::RotatingFrame::for_radius(m.radius);
    const uint64_t t3 = SDL_GetTicksNS();

    // Clouds are decoration: without them the world still runs.
    if (!g_clouds.create(m.radius, m.map_w, m.map_h))
        fprintf(stderr, "load_scene: cloud shaders missing; no clouds\n");
    const uint64_t t4 = SDL_GetTicksNS();

    // Start on a street near the middle of the map, or wherever --pos says.
    double px = 0.5 * m.map_w, py = 0.5 * m.map_h, palt = 0;
    if (g_cam_from_args) {
        px   = g_cam.x;
        py   = g_cam.y;
        palt = g_cam.alt - game::k_eye_height;
    } else if (!game::find_spawn(g_collision, px, py, palt)) {
        fprintf(stderr, "load_scene: no street near the map centre; starting in noclip\n");
        g_noclip  = true;
        g_cam     = {px, py, k_start_altitude};
    }
    g_player = game::spawn_player(g_registry, m.radius, px, py, palt);
    if (!g_noclip) g_cam = g_cam_written = game::player_eye(g_registry, g_player, m.radius);

    const render::WorldStats& st = g_world.stats();
    printf("world: %zu chunks, %llu terrain tris in %u draws, %llu buildings in %u draws, "
           "%zu monuments; loaded in %.2f s, uploaded in %.2f s, collision in %.2f s\n"
           "clouds: %zu clouds, %zu puffs, generated in %.2f s\n",
           m.chunks.size(), static_cast<unsigned long long>(st.terrain_triangles), st.terrain_draws,
           static_cast<unsigned long long>(st.building_instances), st.building_draws,
           world->monument_count, (t1 - t0) * 1e-9, (t2 - t1) * 1e-9, (t3 - t2) * 1e-9,
           g_clouds.cloud_count(), g_clouds.puff_count(), (t4 - t3) * 1e-9);
    return true;
}

// -----------------------------------------------------------------------
// Camera
// -----------------------------------------------------------------------

// Render space is camera-relative with +Y toward the axis, +Z along the axis
// (map +Y) and +X around the circumference (map +X); see shaders/cylinder.sh.
static bx::Vec3 cam_forward()
{
    const float cp = bx::cos(g_cam_pitch);
    return {cp * bx::sin(g_cam_yaw), bx::sin(g_cam_pitch), cp * bx::cos(g_cam_yaw)};
}

// Free-fly movement. A render-space step converts back to map space at the
// camera's own radius: circumferential metres there are R / (R − alt) map metres.
static void update_camera(double dt)
{
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const bx::Vec3 f = cam_forward();
    const bx::Vec3 right{bx::cos(g_cam_yaw), 0.0f, -bx::sin(g_cam_yaw)};

    float mx = 0, my = 0, mz = 0;
    auto add = [&](const bx::Vec3& v, float s) { mx += v.x * s; my += v.y * s; mz += v.z * s; };
    if (keys[SDL_SCANCODE_W]) add(f, 1.0f);
    if (keys[SDL_SCANCODE_S]) add(f, -1.0f);
    if (keys[SDL_SCANCODE_D]) add(right, 1.0f);
    if (keys[SDL_SCANCODE_A]) add(right, -1.0f);
    if (keys[SDL_SCANCODE_SPACE]) my += 1.0f;
    if (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_C]) my -= 1.0f;

    const double len = std::sqrt(double(mx) * mx + double(my) * my + double(mz) * mz);
    if (len < 1e-6) return;
    const double speed = g_fly_speed * (keys[SDL_SCANCODE_LSHIFT] ? 10.0 : 1.0) * dt / len;

    const double R = g_cylinder_radius;
    g_cam.x   = world::wrap_x(g_cam.x + mx * speed * R / (R - g_cam.alt));
    g_cam.alt = bx::clamp(g_cam.alt + my * speed, 1.5, R - 100.0);
    g_cam.y   = bx::clamp(g_cam.y + mz * speed, 0.0, g_map_h);
}

// Walking: feed WASD to the player's controller and put the camera at its eyes.
static void update_player(double dt, bool take_input)
{
    // The console moves the camera (teleport, Camera.x = ...); carry the player along.
    if (g_cam.x != g_cam_written.x || g_cam.y != g_cam_written.y || g_cam.alt != g_cam_written.alt)
        game::place_player(g_registry, g_player, g_cylinder_radius, g_cam.x, g_cam.y,
                           g_cam.alt - game::k_eye_height);

    game::PlayerInput in;
    in.yaw = g_cam_yaw;
    if (take_input) {
        const bool* keys = SDL_GetKeyboardState(nullptr);
        in.forward = float(keys[SDL_SCANCODE_W]) - float(keys[SDL_SCANCODE_S]);
        in.right   = float(keys[SDL_SCANCODE_D]) - float(keys[SDL_SCANCODE_A]);
        in.sprint  = keys[SDL_SCANCODE_LSHIFT];
    }
    game::update_player(g_registry, g_player, in, g_collision, g_frame, g_map_h, dt);
    g_cam = g_cam_written = game::player_eye(g_registry, g_player, g_cylinder_radius);
}

static void set_noclip(bool on)
{
    if (g_noclip == on || g_player == entt::null) return;
    // Leaving noclip drops the player in where the camera is.
    if (!on)
        game::place_player(g_registry, g_player, g_cylinder_radius, g_cam.x, g_cam.y,
                           g_cam.alt - game::k_eye_height);
    g_noclip = on;
}

// -----------------------------------------------------------------------
// Scene rendering (called each Game frame)
// -----------------------------------------------------------------------

static void render_scene()
{
    // The eye is always the render-space origin; the world is built around it.
    // "Up" is the local direction toward the axis (+Y), so the floor stays down.
    float view[16];
    float proj[16];

    const bx::Vec3 eye{0.0f, 0.0f, 0.0f};
    const bx::Vec3 at = cam_forward();
    const float sp = bx::sin(g_cam_pitch);
    const float cp = bx::cos(g_cam_pitch);
    const float sy = bx::sin(g_cam_yaw);
    const float cy = bx::cos(g_cam_yaw);
    // Up is world +Y tilted by pitch, so it stays perpendicular to the look
    // direction and mtxLookAt doesn't degenerate at straight up/down.
    const bx::Vec3 up {-sp * sy, cp, -sp * cy};
    bx::mtxLookAt(view, eye, at, up);

    // The eye can be a capsule radius (0.3 m) from a wall, so the near plane must be
    // well inside that. Reversed-Z float depth doesn't mind; plain GL depth does,
    // and accepts distant z-fighting as the price.
    constexpr float k_near    = 0.05f;
    constexpr float k_near_gl = 0.1f;

    // Reversed-Z with an infinite far plane where depth is [0, 1]: terrain layers
    // sit 5 cm apart and are visible 8 km away, far past what standard depth
    // resolves. OpenGL's [-1, 1] range defeats the trick, so it gets a plain
    // projection with a pushed-out near plane instead.
    const bool  reversed_z = !bgfx::getCaps()->homogeneousDepth;
    const float aspect = static_cast<float>(k_width) / static_cast<float>(k_height);
    if (reversed_z)
        bx::mtxProjInf(proj, 70.0f, aspect, k_near, false, bx::Handedness::Left, bx::NearFar::Reverse);
    else
        bx::mtxProj(proj, 70.0f, aspect, k_near_gl, 40000.0f, true);

    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, render::k_fog_rgba,
                       reversed_z ? 0.0f : 1.0f, 0);
    // setViewTransform is per-view and persists for all draws to view 0 this frame.
    bgfx::setViewTransform(0, view, proj);
    g_world.cloud_shadow.texture  = g_clouds.enabled ? g_clouds.shadow_texture()
                                                     : bgfx::TextureHandle BGFX_INVALID_HANDLE;
    g_world.cloud_shadow.offset_x = g_clouds.offset_x();
    g_world.cloud_shadow.offset_y = g_clouds.offset_y();
    g_world.submit(0, g_cam, reversed_z);
    // Blended, so bgfx sorts it after the opaque world within the view.
    g_clouds.submit(0, g_cam, reversed_z, g_world.fog);
}

// -----------------------------------------------------------------------
// Scene teardown
// -----------------------------------------------------------------------

static void free_scene()
{
    g_clouds.destroy();
    g_world.destroy();
    g_scene_loaded = false;
}

// -----------------------------------------------------------------------
// Command line
// -----------------------------------------------------------------------

//   --shot <file.png>         render the world once, save a screenshot, exit
//   --pos <x> <y> <alt>       start position in map metres
//   --view <yaw> <pitch>      start orientation in degrees
//   --noclip                  start free-flying instead of walking
//   --console <ruby>          open the console and run a line (repeatable)
//   --exec <ruby>             run a line without opening the console (repeatable)
static bool parse_args(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!strcmp(a, "--shot") && i + 1 < argc) {
            g_shot_path = argv[++i];
        } else if (!strcmp(a, "--pos") && i + 3 < argc) {
            g_cam.x   = atof(argv[++i]);
            g_cam.y   = atof(argv[++i]);
            g_cam.alt = atof(argv[++i]);
            g_cam_from_args = true;
        } else if (!strcmp(a, "--view") && i + 2 < argc) {
            g_cam_yaw   = bx::toRad(static_cast<float>(atof(argv[++i])));
            g_cam_pitch = bx::clamp(bx::toRad(static_cast<float>(atof(argv[++i]))),
                                    -k_pitch_max, k_pitch_max);
        } else if (!strcmp(a, "--noclip")) {
            g_noclip = true;
        } else if (!strcmp(a, "--console") && i + 1 < argc) {
            g_console_lines.push_back({argv[++i], true});
        } else if (!strcmp(a, "--exec") && i + 1 < argc) {
            g_console_lines.push_back({argv[++i], false});
        } else {
            fprintf(stderr, "usage: %s [--shot file.png] [--pos x y alt] [--view yaw pitch] "
                            "[--noclip] [--console ruby]... [--exec ruby]...\n",
                    argv[0]);
            return false;
        }
    }
    return true;
}

// -----------------------------------------------------------------------
// Entry point
// -----------------------------------------------------------------------

int main(int argc, char* argv[])
{
    if (!parse_args(argc, argv))
        return 1;

    // ---------------------------------------------------------------
    // SDL3: window
    // ---------------------------------------------------------------
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow(
        "Eschaton",
        static_cast<int>(k_width), static_cast<int>(k_height),
        SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // ---------------------------------------------------------------
    // bgfx: native handle + init
    // ---------------------------------------------------------------
    bgfx::Init init;
    init.type              = bgfx::RendererType::Count; // auto-select
    init.swapChain.width  = k_width;
    init.swapChain.height = k_height;
    init.reset             = BGFX_RESET_VSYNC | BGFX_RESET_MSAA_X4;
    // 32-bit float depth for reversed-Z (see render_scene).
    init.swapChain.formatDepthStencil = bgfx::TextureFormat::D32F;
    init.callback          = &g_bgfx_callback;

    SDL_PropertiesID props = SDL_GetWindowProperties(window);

#if defined(SDL_PLATFORM_LINUX)
    if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0) {
        // Must set the handle type explicitly; bgfx defaults to Xlib otherwise.
        init.platformData.type = bgfx::NativeWindowHandleType::Wayland;
        init.swapChain.ndt =
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        init.swapChain.nwh =
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        init.swapChain.ndt =
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        init.swapChain.nwh = reinterpret_cast<void*>(
            SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    }
#elif defined(SDL_PLATFORM_WINDOWS)
    init.swapChain.nwh =
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(SDL_PLATFORM_MACOS)
    init.swapChain.nwh =
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
#endif

    if (!bgfx::init(init)) {
        fprintf(stderr, "bgfx::init failed\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Enable the debug-text overlay (used for Title and Menu screens).
    bgfx::setDebug(BGFX_DEBUG_TEXT);

    bool running = true;
    if (g_console.create()) {
        g_debug_host = {&g_cam, &g_cam_yaw, &g_cam_pitch, k_pitch_max, &g_fly_speed,
                        g_cylinder_radius, g_map_h, &g_world, &running,
                        &g_registry, &g_player, &g_noclip, &g_collision, &g_frame, &g_clouds};
        debug::install_bindings(g_console, g_debug_host);
    }

    // ---------------------------------------------------------------
    // UI state
    // ---------------------------------------------------------------
    Screen   screen      = g_shot_path ? Screen::Game : Screen::Title;
    uint64_t title_start = SDL_GetTicks();
    int      menu_sel    = 0;   // 0 = PLAY, 1 = QUIT

    static const char* k_menu_items[] = { "PLAY", "QUIT" };
    static constexpr int k_menu_count = 2;

    // ---------------------------------------------------------------
    // Game loop
    // ---------------------------------------------------------------
    uint64_t last_ticks = SDL_GetTicksNS();
    int      shot_frame = 0;   // world frames rendered in --shot mode
    while (running) {
        const uint64_t now_ticks = SDL_GetTicksNS();
        const double   dt = (now_ticks - last_ticks) * 1e-9;
        last_ticks = now_ticks;

        // -- Input --------------------------------------------------
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
            }
            // The console sees input first: its toggle works on every screen, and
            // while it is open nothing reaches the game.
            if (g_console.handle_event(ev, window))
                continue;
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Scancode sc = ev.key.scancode;
                switch (screen) {

                    case Screen::Title:
                        // Any key skips the timed title card.
                        screen = Screen::Menu;
                        break;

                    case Screen::Menu:
                        if (sc == SDL_SCANCODE_UP)
                            menu_sel = (menu_sel - 1 + k_menu_count) % k_menu_count;
                        else if (sc == SDL_SCANCODE_DOWN)
                            menu_sel = (menu_sel + 1) % k_menu_count;
                        else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
                            if (menu_sel == 0) {
                                screen = Screen::Game;
                                // Relative mode, not just a grab: a grab only confines
                                // the pointer, and once it pins against the window edge
                                // (always, on Wayland) the deltas stop and yaw stalls.
                                SDL_SetWindowRelativeMouseMode(window, true);
                            } else {
                                running = false;         // QUIT
                            }
                        }
                        break;

                    case Screen::Game:
                        // ESC suspends to menu without resetting game state.
                        if (sc == SDL_SCANCODE_ESCAPE) {
                            screen = Screen::Menu;
                            SDL_SetWindowRelativeMouseMode(window, false);
                        } else if (sc == SDL_SCANCODE_F && !ev.key.repeat) {
                            set_noclip(!g_noclip);
                        } else if (sc == SDL_SCANCODE_SPACE && !ev.key.repeat && !g_noclip
                                   && g_player != entt::null) {
                            game::queue_jump(g_registry, g_player);
                        }
                        break;
                }
            }
            // Mouselook: accumulate relative mouse motion while in-game.
            // --shot frames are scripted; a live mouse must not swing the camera.
            if (ev.type == SDL_EVENT_MOUSE_MOTION && screen == Screen::Game && !g_shot_path) {
                g_cam_yaw   += ev.motion.xrel * k_mouse_sens;
                g_cam_pitch -= ev.motion.yrel * k_mouse_sens;
                g_cam_yaw    = std::remainder(g_cam_yaw, bx::kPi2);   // keep in [-π, π]
                if (g_cam_pitch >  k_pitch_max) g_cam_pitch =  k_pitch_max;
                if (g_cam_pitch < -k_pitch_max) g_cam_pitch = -k_pitch_max;
            }
        }

        // Auto-advance title card after 3 seconds.
        if (screen == Screen::Title && SDL_GetTicks() - title_start >= 3000)
            screen = Screen::Menu;

        // -- Render -------------------------------------------------
        bgfx::setViewClear(0,
            BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
            0x87CEEBff,  // sky-blue, matches fog color
            1.0f, 0);
        bgfx::setViewRect(0, 0, 0,
            static_cast<uint16_t>(k_width),
            static_cast<uint16_t>(k_height));
        bgfx::touch(0);
        bgfx::dbgTextClear();

        switch (screen) {

            case Screen::Title: {
                const char* txt = "ESCHATON";
                bgfx::dbgTextPrintf(
                    cx(static_cast<uint16_t>(strlen(txt))),
                    k_rows / 2,
                    0x0F,   // bright white on black
                    txt);
                break;
            }

            case Screen::Menu: {
                for (int i = 0; i < k_menu_count; ++i) {
                    bgfx::dbgTextPrintf(
                        cx(static_cast<uint16_t>(strlen(k_menu_items[i]))),
                        k_rows / 2 - 1 + i * 2,
                        (i == menu_sel) ? 0x0F : 0x07, // selected=white, normal=gray
                        k_menu_items[i]);
                }
                break;
            }

            case Screen::Game:
                // Show the loading card for a couple of frames so it actually
                // reaches the screen, then load (blocking) on the next one.
                if (!g_scene_loaded && !g_load_failed) {
                    const char* txt = "LOADING WORLD...";
                    bgfx::dbgTextPrintf(cx(static_cast<uint16_t>(strlen(txt))), k_rows / 2, 0x0F, txt);
                    if (++g_load_wait > 2) {
                        g_scene_loaded = load_scene();
                        g_load_failed  = !g_scene_loaded;
                        // After the load, so commands see the manifest's map bounds.
                        for (const StartupLine& line : g_console_lines)
                            g_console.run_line(line.code, window, line.show);
                        last_ticks     = SDL_GetTicksNS();   // don't fly off on the long frame
                    }
                } else if (g_load_failed) {
                    const char* txt = "WORLD FAILED TO LOAD (see console)";
                    bgfx::dbgTextPrintf(cx(static_cast<uint16_t>(strlen(txt))), k_rows / 2, 0x0C, txt);
                    if (g_shot_path) running = false;
                }

                if (g_scene_loaded) {
                    g_clouds.update(dt);
                    // The player keeps simulating (and falling) while the console is
                    // open; it just doesn't hear the keyboard.
                    const bool take_input = !g_shot_path && !g_console.is_open();
                    if (g_noclip) {
                        if (take_input) update_camera(dt);
                    } else {
                        update_player(dt, take_input);
                    }
                    render_scene();

                    const bgfx::Stats* st = bgfx::getStats();
                    const double cpu_ms = st->cpuTimerFreq
                        ? 1000.0 * double(st->cpuTimeFrame) / double(st->cpuTimerFreq) : 0.0;
                    const double gpu_ms = st->gpuTimerFreq
                        ? 1000.0 * double(st->gpuTimeEnd - st->gpuTimeBegin) / double(st->gpuTimerFreq) : 0.0;
                    bgfx::dbgTextPrintf(1, 1, 0x0F,
                        "x %7.1f  y %7.1f  alt %6.1f m   yaw %4.0f  pitch %3.0f   "
                        "frame %5.2f ms  gpu %5.2f ms  %u draws",
                        g_cam.x, g_cam.y, g_cam.alt, bx::toDeg(g_cam_yaw), bx::toDeg(g_cam_pitch),
                        cpu_ms, gpu_ms, st->numDraw);
                    if (g_noclip) {
                        bgfx::dbgTextPrintf(1, 2, 0x07, "NOCLIP  WASD fly, Space/C up/down, Shift fast, F walk, Esc menu");
                    } else {
                        const game::PlayerView pv = game::player_view(g_registry, g_player, g_cylinder_radius);
                        bgfx::dbgTextPrintf(1, 2, 0x07, "WALK %s %4.1f m/s  WASD move, Space jump, Shift sprint, "
                                            "F noclip, Esc menu",
                                            pv.grounded ? "grounded" : "airborne ",
                                            std::hypot(pv.v[0], pv.v[1]));
                    }

                    // --shot: let a few frames settle, capture, wait for the
                    // render thread to hand the image back, then exit.
                    if (g_shot_path) {
                        ++shot_frame;
                        if (shot_frame == 4)
                            bgfx::requestScreenShot(bgfx::FrameBufferHandle BGFX_INVALID_HANDLE, g_shot_path);
                        if (g_bgfx_callback.shot_done || shot_frame > 60)
                            running = false;
                    }
                }
                break;
        }

        // View 1: drawn after the scene, so the console backdrop covers it.
        g_console.draw(1, static_cast<uint16_t>(k_width), static_cast<uint16_t>(k_height), k_cols, k_rows);

        bgfx::frame();
    }

    // ---------------------------------------------------------------
    // Shutdown
    // ---------------------------------------------------------------
    free_scene();
    g_console.destroy();
    bgfx::shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
