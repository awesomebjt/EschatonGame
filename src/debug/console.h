#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include <bgfx/bgfx.h>

union SDL_Event;
struct SDL_Window;
struct mrb_state;
struct mrb_ccontext;
struct mrb_parser_state;

namespace debug {

// Drop-down Ruby console (Shift+`), drawn over everything in the top half of the
// screen with a translucent backdrop. Lines are evaluated in one persistent
// mruby VM the way mirb does it, so locals survive between lines and an
// unfinished block (`def`, `do`, an open string) continues on the next line.
class Console
{
public:
    bool create();
    void destroy();

    // Runs Ruby at top level without echoing it, e.g. helper definitions the
    // game adds after create(). Errors go to the scrollback.
    void eval_startup(const char* code);

    // Runs `line` exactly as if it had been typed; `open` shows the console too.
    void run_line(std::string_view line, SDL_Window* window, bool open = true);

    bool       is_open() const { return m_open; }
    mrb_state* vm() const { return m_mrb; }

    // Feed every SDL event through here first. Returns true if the console took
    // it (the toggle key always; all keyboard and mouse input while open).
    bool handle_event(const SDL_Event& ev, SDL_Window* window);

    // Draws the backdrop to `view` (an otherwise unused view after the scene)
    // and the text with bgfx debug text. No-op while closed.
    void draw(bgfx::ViewId view, uint16_t width, uint16_t height, uint16_t cols, uint16_t rows);

    // Appends output to the scrollback; text is split on '\n' and a trailing
    // fragment stays open for the next write, like a terminal.
    void write(std::string_view text, uint8_t attr = k_attr_output);

    void clear();

    static constexpr uint8_t k_attr_output = 0x0F;   // bright white
    static constexpr uint8_t k_attr_echo   = 0x07;   // grey
    static constexpr uint8_t k_attr_result = 0x0B;   // bright cyan
    static constexpr uint8_t k_attr_error  = 0x0C;   // bright red

private:
    struct Line
    {
        std::string text;
        uint8_t     attr;
    };

    void set_open(bool open, SDL_Window* window);
    void key_down(const SDL_Event& ev, SDL_Window* window);
    void insert(std::string_view text);
    void submit();
    void eval(const std::string& code);
    void push_line(std::string text, uint8_t attr);
    void flush_partial();

    mrb_state*    m_mrb   = nullptr;
    mrb_ccontext* m_cxt   = nullptr;
    int           m_arena = 0;       // GC arena index to restore after each eval
    int           m_stack_keep = 0;  // locals the top-level env carries between evals

    bool m_open = false;
    bool m_restore_relative_mouse = false;
    bool m_swallow_toggle_text = false;   // drop the '~' the toggle key may also type

    std::deque<Line>         m_lines;
    std::string              m_partial;   // output written without a newline yet
    uint8_t                  m_partial_attr = k_attr_output;
    int                      m_scroll = 0;   // rows scrolled back from the bottom
    std::string              m_input;
    size_t                   m_cursor = 0;
    std::string              m_pending;  // earlier lines of an unfinished block
    std::vector<std::string> m_history;
    size_t                   m_history_pos = 0;  // == size() when editing a fresh line

    bgfx::ProgramHandle m_prog    = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_color   = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout  m_layout;
};

} // namespace debug
