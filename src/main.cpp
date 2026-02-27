#include <cstdio>
#include <cstring>
#include <vector>

#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <SDL3/SDL.h>
#include <entt/entt.hpp>

#include "cgltf.h"

static constexpr uint32_t k_width  = 1280;
static constexpr uint32_t k_height =  720;

// bgfx's embedded debug font is 8 px wide × 16 px tall.
static constexpr uint16_t k_cols = k_width  / 8;   // 160
static constexpr uint16_t k_rows = k_height / 16;  // 45

// Return the column that left-aligns a string of `len` chars at the centre.
static uint16_t cx(uint16_t len) { return (k_cols - len) / 2; }

enum class Screen { Title, Menu, Game };

// -----------------------------------------------------------------------
// Scene types
// -----------------------------------------------------------------------

struct Primitive
{
    bgfx::VertexBufferHandle vbh         = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle  ibh         = BGFX_INVALID_HANDLE;
    uint32_t                 num_indices = 0;
};

// Scene state — created once on first Game entry, destroyed on shutdown.
static bgfx::ProgramHandle    g_program    = BGFX_INVALID_HANDLE;
static bgfx::UniformHandle    g_u_ambient  = BGFX_INVALID_HANDLE;
static std::vector<Primitive> g_primitives;
static bool                   g_scene_loaded = false;

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
// GLB scene loading
// -----------------------------------------------------------------------

static bool load_scene()
{
    static const char* glb_path = ASSETS_DIR "/cylinder1.glb";

    cgltf_options opts{};
    cgltf_data*   data   = nullptr;

    if (cgltf_parse_file(&opts, glb_path, &data) != cgltf_result_success)
    {
        fprintf(stderr, "load_scene: cgltf_parse_file failed: %s\n", glb_path);
        return false;
    }

    if (cgltf_load_buffers(&opts, data, glb_path) != cgltf_result_success)
    {
        fprintf(stderr, "load_scene: cgltf_load_buffers failed\n");
        cgltf_free(data);
        return false;
    }

    bgfx::VertexLayout layout;
    layout.begin()
          .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
          .end();

    for (cgltf_size mi = 0; mi < data->meshes_count; ++mi)
    {
        const cgltf_mesh& mesh = data->meshes[mi];
        for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi)
        {
            const cgltf_primitive& prim = mesh.primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles || !prim.indices)
                continue;

            // Find the POSITION accessor.
            const cgltf_accessor* pos_acc = nullptr;
            for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai)
            {
                if (prim.attributes[ai].type == cgltf_attribute_type_position)
                {
                    pos_acc = prim.attributes[ai].data;
                    break;
                }
            }
            if (!pos_acc)
                continue;

            const uint32_t num_verts = static_cast<uint32_t>(pos_acc->count);
            const uint32_t num_idx   = static_cast<uint32_t>(prim.indices->count);
            const bool     idx32     = (num_verts > 65535);

            // Upload vertex positions.
            const bgfx::Memory* vb_mem = bgfx::alloc(num_verts * 3 * sizeof(float));
            auto* verts = reinterpret_cast<float*>(vb_mem->data);
            for (uint32_t v = 0; v < num_verts; ++v)
                cgltf_accessor_read_float(pos_acc, v, verts + v * 3, 3);

            // Upload indices (16-bit when possible to save bandwidth).
            const uint32_t        idx_stride = idx32 ? 4u : 2u;
            const bgfx::Memory*   ib_mem     = bgfx::alloc(num_idx * idx_stride);
            if (idx32)
            {
                auto* idx = reinterpret_cast<uint32_t*>(ib_mem->data);
                for (uint32_t i = 0; i < num_idx; ++i)
                    idx[i] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i));
            }
            else
            {
                auto* idx = reinterpret_cast<uint16_t*>(ib_mem->data);
                for (uint32_t i = 0; i < num_idx; ++i)
                    idx[i] = static_cast<uint16_t>(cgltf_accessor_read_index(prim.indices, i));
            }

            Primitive p;
            p.vbh         = bgfx::createVertexBuffer(vb_mem, layout);
            p.ibh         = bgfx::createIndexBuffer(ib_mem, idx32 ? BGFX_BUFFER_INDEX32 : 0);
            p.num_indices = num_idx;
            g_primitives.push_back(p);
        }
    }

    cgltf_free(data);

    if (g_primitives.empty())
    {
        fprintf(stderr, "load_scene: no usable primitives in %s\n", glb_path);
        return false;
    }

    // Build shader program.
    const bgfx::ShaderHandle vs = load_shader_binary("vs_mesh.sc");
    const bgfx::ShaderHandle fs = load_shader_binary("fs_mesh.sc");
    if (!bgfx::isValid(vs) || !bgfx::isValid(fs))
    {
        if (bgfx::isValid(vs)) bgfx::destroy(vs);
        if (bgfx::isValid(fs)) bgfx::destroy(fs);
        return false;
    }

    g_program   = bgfx::createProgram(vs, fs, /*destroyShaders=*/true);
    g_u_ambient = bgfx::createUniform("u_ambient", bgfx::UniformType::Vec4);

    fprintf(stderr, "load_scene: loaded %zu primitive(s) from %s\n",
            g_primitives.size(), glb_path);
    return true;
}

// -----------------------------------------------------------------------
// Scene rendering (called each Game frame)
// -----------------------------------------------------------------------

static void render_scene()
{
    // Camera: sit at the cylinder's geometric centre, looking down the +Z axis
    // (the long axis of the cylinder as exported from Blender).
    float view[16];
    float proj[16];

    const bx::Vec3 eye{0.0f, 0.0f, 0.0f};
    const bx::Vec3 at {0.0f, 0.0f, 1.0f};
    const bx::Vec3 up {0.0f, 1.0f, 0.0f};
    bx::mtxLookAt(view, eye, at, up);

    const float aspect = static_cast<float>(k_width) / static_cast<float>(k_height);
    bx::mtxProj(proj, 70.0f, aspect, 0.05f, 1000.0f,
                bgfx::getCaps()->homogeneousDepth);

    // setViewTransform is per-view and persists for all draws to view 0 this frame.
    bgfx::setViewTransform(0, view, proj);

    // Ambient-only cool white light — no shading, just a flat interior colour.
    const float ambient[4] = {0.65f, 0.65f, 0.70f, 1.0f};

    // Identity model transform: GLB is already at world origin.
    float model[16];
    bx::mtxIdentity(model);

    // Disable backface culling — we are inside the cylinder, so its faces
    // appear with reversed winding from the camera's perspective.
    const uint64_t draw_state = 0
        | BGFX_STATE_WRITE_RGB
        | BGFX_STATE_WRITE_A
        | BGFX_STATE_WRITE_Z
        | BGFX_STATE_DEPTH_TEST_LESS
        | BGFX_STATE_MSAA;

    for (const Primitive& prim : g_primitives)
    {
        bgfx::setTransform(model);
        bgfx::setVertexBuffer(0, prim.vbh);
        bgfx::setIndexBuffer(prim.ibh);
        bgfx::setState(draw_state);
        bgfx::setUniform(g_u_ambient, ambient);
        bgfx::submit(0, g_program);
    }
}

// -----------------------------------------------------------------------
// Scene teardown
// -----------------------------------------------------------------------

static void free_scene()
{
    for (const Primitive& p : g_primitives)
    {
        bgfx::destroy(p.vbh);
        bgfx::destroy(p.ibh);
    }
    g_primitives.clear();

    if (bgfx::isValid(g_u_ambient)) bgfx::destroy(g_u_ambient);
    if (bgfx::isValid(g_program))   bgfx::destroy(g_program);

    g_u_ambient    = BGFX_INVALID_HANDLE;
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
    init.resolution.width  = k_width;
    init.resolution.height = k_height;
    init.resolution.reset  = BGFX_RESET_VSYNC;

    SDL_PropertiesID props = SDL_GetWindowProperties(window);

#if defined(SDL_PLATFORM_LINUX)
    if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0) {
        // Must set the handle type explicitly; bgfx defaults to Xlib otherwise.
        init.platformData.type = bgfx::NativeWindowHandleType::Wayland;
        init.platformData.ndt =
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        init.platformData.nwh =
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        init.platformData.ndt =
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        init.platformData.nwh = reinterpret_cast<void*>(
            SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    }
#elif defined(SDL_PLATFORM_WINDOWS)
    init.platformData.nwh =
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(SDL_PLATFORM_MACOS)
    init.platformData.nwh =
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
                            if (menu_sel == 0)
                                screen = Screen::Game;   // PLAY
                            else
                                running = false;         // QUIT
                        }
                        break;

                    case Screen::Game:
                        // ESC suspends to menu without resetting game state.
                        if (sc == SDL_SCANCODE_ESCAPE)
                            screen = Screen::Menu;
                        break;
                }
            }
        }

        // Auto-advance title card after 3 seconds.
        if (screen == Screen::Title && SDL_GetTicks() - title_start >= 3000)
            screen = Screen::Menu;

        // -- Render -------------------------------------------------
        bgfx::setViewClear(0,
            BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
            0x000010ff,  // very dark blue-black
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
