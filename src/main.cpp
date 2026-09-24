#include <cstdio>
#include <cstring>

#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <SDL3/SDL.h>
#include <entt/entt.hpp>

static constexpr uint32_t k_width  = 1280;
static constexpr uint32_t k_height =  720;

// bgfx's embedded debug font is 8 px wide × 16 px tall.
static constexpr uint16_t k_cols = k_width  / 8;   // 160
static constexpr uint16_t k_rows = k_height / 16;  // 45

// Return the column that left-aligns a string of `len` chars at the centre.
static uint16_t cx(uint16_t len) { return (k_cols - len) / 2; }

enum class Screen { Title, Menu, Game };

// Scene state — created once on first Game entry, destroyed on shutdown.
static bgfx::ProgramHandle g_program      = BGFX_INVALID_HANDLE;
static bgfx::UniformHandle g_s_texColor   = BGFX_INVALID_HANDLE;
static bgfx::TextureHandle g_white_tex    = BGFX_INVALID_HANDLE;
static bool                g_scene_loaded = false;

// Camera orientation controlled by mouselook.
// Yaw rotates left/right around the hub-pointing (Y) axis and wraps freely.
// Pitch tilts up/down, clamped to straight down / straight up.
static float g_cam_yaw              = 0.0f;
static float g_cam_pitch            = 0.0f;
static constexpr float k_mouse_sens = 0.002f;          // radians per pixel
static constexpr float k_pitch_max  = bx::kPiHalf;

// -----------------------------------------------------------------------
// Shader loading helpers
// -----------------------------------------------------------------------

// Map bgfx renderer type to the subdirectory name used by bgfx_compile_shaders.
static const char* shader_backend()
{
    switch (bgfx::getRendererType())
    {
        case bgfx::RendererType::Vulkan:     return "spirv";
        case bgfx::RendererType::OpenGL:     return "glsl";
        case bgfx::RendererType::OpenGLES:   return "essl";
        case bgfx::RendererType::Direct3D11: return "dx11";
        case bgfx::RendererType::Direct3D12: return "dx12";
        case bgfx::RendererType::Metal:      return "metal";
        default:                              return "spirv";
    }
}

// Load a compiled shader binary.  `sc_filename` is the .sc source name
// (e.g. "vs_mesh.sc") so the path matches bgfx_compile_shaders output:
//   SHADER_DIR/<backend>/<sc_filename>.bin
static bgfx::ShaderHandle load_shader_binary(const char* sc_filename)
{
    char path[512];
    snprintf(path, sizeof(path), SHADER_DIR "/%s/%s.bin", shader_backend(), sc_filename);

    FILE* f = fopen(path, "rb");
    if (!f)
    {
        fprintf(stderr, "load_shader_binary: cannot open %s\n", path);
        return BGFX_INVALID_HANDLE;
    }

    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    const bgfx::Memory* mem = bgfx::alloc(static_cast<uint32_t>(size + 1));
    fread(mem->data, 1, static_cast<size_t>(size), f);
    mem->data[size] = '\0';
    fclose(f);

    return bgfx::createShader(mem);
}

// -----------------------------------------------------------------------
// Scene setup
// -----------------------------------------------------------------------

// Builds the shading resources (fog shader program, sampler, fallback
// texture) that scene geometry will draw with. The scene has no geometry yet.
static bool load_scene()
{
    const bgfx::ShaderHandle vs = load_shader_binary("vs_mesh.sc");
    const bgfx::ShaderHandle fs = load_shader_binary("fs_mesh.sc");
    if (!bgfx::isValid(vs) || !bgfx::isValid(fs))
    {
        if (bgfx::isValid(vs)) bgfx::destroy(vs);
        if (bgfx::isValid(fs)) bgfx::destroy(fs);
        return false;
    }

    g_program    = bgfx::createProgram(vs, fs, /*destroyShaders=*/true);
    g_s_texColor = bgfx::createUniform("s_texColor", bgfx::UniformType::Sampler);

    // 1x1 white fallback texture for geometry without materials.
    const uint32_t white = 0xFFFFFFFF;
    g_white_tex = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, 0,
                                        bgfx::copy(&white, 4));

    return true;
}

// -----------------------------------------------------------------------
// Scene rendering (called each Game frame)
// -----------------------------------------------------------------------

static void render_scene()
{
    // Camera: sit at the cylinder's geometric centre.
    // "Up" always points toward the hub (world +Y), ensuring the floor stays down.
    // Look direction is derived from yaw (left/right) and pitch (up/down).
    float view[16];
    float proj[16];

    const bx::Vec3 eye{0.0f, -3995.0f, 0.0f};
    const float cp = bx::cos(g_cam_pitch);
    const float sp = bx::sin(g_cam_pitch);
    const float cy = bx::cos(g_cam_yaw);
    const float sy = bx::sin(g_cam_yaw);
    const bx::Vec3 at {
        eye.x + cp * sy,   // yaw=0 → looking along +Z; yaw=90° → +X
        eye.y + sp,
        eye.z + cp * cy
    };
    // Up is world +Y tilted by pitch, so it stays perpendicular to the look
    // direction and mtxLookAt doesn't degenerate at straight up/down.
    const bx::Vec3 up {-sp * sy, cp, -sp * cy};
    bx::mtxLookAt(view, eye, at, up);

    const float aspect = static_cast<float>(k_width) / static_cast<float>(k_height);
    bx::mtxProj(proj, 70.0f, aspect, 1.0f, 100000.0f,
                bgfx::getCaps()->homogeneousDepth);

    // setViewTransform is per-view and persists for all draws to view 0 this frame.
    bgfx::setViewTransform(0, view, proj);
}

// -----------------------------------------------------------------------
// Scene teardown
// -----------------------------------------------------------------------

static void free_scene()
{
    if (bgfx::isValid(g_s_texColor)) bgfx::destroy(g_s_texColor);
    if (bgfx::isValid(g_white_tex))  bgfx::destroy(g_white_tex);
    if (bgfx::isValid(g_program))    bgfx::destroy(g_program);

    g_s_texColor   = BGFX_INVALID_HANDLE;
    g_white_tex    = BGFX_INVALID_HANDLE;
    g_program      = BGFX_INVALID_HANDLE;
    g_scene_loaded = false;
}

// -----------------------------------------------------------------------
// Entry point
// -----------------------------------------------------------------------

int main(int /*argc*/, char* /*argv*/[])
{
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
    init.reset             = BGFX_RESET_VSYNC;

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

    // ---------------------------------------------------------------
    // ECS registry (populated in later steps)
    // ---------------------------------------------------------------
    entt::registry registry;
    (void)registry;

    // ---------------------------------------------------------------
    // UI state
    // ---------------------------------------------------------------
    Screen   screen      = Screen::Title;
    uint64_t title_start = SDL_GetTicks();
    int      menu_sel    = 0;   // 0 = PLAY, 1 = QUIT

    static const char* k_menu_items[] = { "PLAY", "QUIT" };
    static constexpr int k_menu_count = 2;

    // ---------------------------------------------------------------
    // Game loop
    // ---------------------------------------------------------------
    bool running = true;
    while (running) {

        // -- Input --------------------------------------------------
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
            }
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
                                SDL_HideCursor();
                                SDL_SetWindowMouseGrab(window, true);
                            } else {
                                running = false;         // QUIT
                            }
                        }
                        break;

                    case Screen::Game:
                        // ESC suspends to menu without resetting game state.
                        if (sc == SDL_SCANCODE_ESCAPE) {
                            screen = Screen::Menu;
                            SDL_SetWindowMouseGrab(window, false);
                            SDL_ShowCursor();
                        }
                        break;
                }
            }
            // Mouselook: accumulate relative mouse motion while in-game.
            if (ev.type == SDL_EVENT_MOUSE_MOTION && screen == Screen::Game) {
                g_cam_yaw   += ev.motion.xrel * k_mouse_sens;
                g_cam_pitch -= ev.motion.yrel * k_mouse_sens;
                if (g_cam_yaw >  bx::kPi) g_cam_yaw -= bx::kPi2;
                if (g_cam_yaw < -bx::kPi) g_cam_yaw += bx::kPi2;
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
                // Load the scene on the first Game frame.
                if (!g_scene_loaded)
                    g_scene_loaded = load_scene();

                if (g_scene_loaded)
                    render_scene();
                break;
        }

        bgfx::frame();
    }

    // ---------------------------------------------------------------
    // Shutdown
    // ---------------------------------------------------------------
    free_scene();
    bgfx::shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
