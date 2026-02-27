#include <cstdio>
#include <cstring>

#include <bgfx/bgfx.h>
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

    // Enable the debug-text overlay (used for all UI at this stage).
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
                // Gameplay rendering added in the next step.
                break;
        }

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
