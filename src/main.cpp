#include <cstdio>

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <SDL3/SDL.h>
#include <entt/entt.hpp>

static constexpr uint32_t k_width  = 1280;
static constexpr uint32_t k_height =  720;

int main(int /*argc*/, char* /*argv*/[])
{
    // ---------------------------------------------------------------
    // SDL3: create a window
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
    // bgfx: supply the native window/display handle, then initialise
    // ---------------------------------------------------------------
    bgfx::Init init;
    init.type              = bgfx::RendererType::Count; // auto-select backend
    init.resolution.width  = k_width;
    init.resolution.height = k_height;
    init.resolution.reset  = BGFX_RESET_VSYNC;

    SDL_PropertiesID props = SDL_GetWindowProperties(window);

#if defined(SDL_PLATFORM_LINUX)
    if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0) {
        init.platformData.type = bgfx::NativeWindowHandleType::Wayland;
        init.platformData.ndt =
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        init.platformData.nwh =
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        // X11
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

    // ---------------------------------------------------------------
    // ECS registry (populated in later steps)
    // ---------------------------------------------------------------
    entt::registry registry;
    (void)registry;

    // ---------------------------------------------------------------
    // Game loop
    // ---------------------------------------------------------------
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT)
                running = false;
        }

        // View 0: clear to near-black (deep space).
        bgfx::setViewClear(0,
            BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
            0x000010ff,  // RGBA — very dark blue-black
            1.0f, 0);
        bgfx::setViewRect(0, 0, 0,
            static_cast<uint16_t>(k_width),
            static_cast<uint16_t>(k_height));
        bgfx::touch(0); // submit view 0 even with no draw calls

        bgfx::frame();
    }

    // ---------------------------------------------------------------
    // Shutdown
    // ---------------------------------------------------------------
    bgfx::shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
