#include "debug/console.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <bx/math.h>
#include <SDL3/SDL.h>

#include <mruby.h>
#include <mruby/compile.h>
#include <mruby/error.h>
#include <mruby/proc.h>
#include <mruby/string.h>

#include "render/shader.h"

namespace debug {

namespace {

constexpr size_t k_max_lines   = 2000;
constexpr size_t k_max_history = 200;

// Backdrop: near-black with a cool tint, translucent enough to keep the scene readable.
constexpr float k_backdrop[4] = {0.02f, 0.03f, 0.05f, 0.72f};

// Output routing. mruby's own print/puts/p write to stdout; these replace them
// so everything a command prints lands in the scrollback instead.
constexpr const char* k_prelude = R"RUBY(
module Kernel
  private

  def print(*args)
    args.each { |a| __console_write(a.to_s) }
    nil
  end

  def puts(*args)
    if args.empty?
      __console_write("\n")
      return nil
    end
    args.flatten.each do |a|
      s = a.nil? ? "" : a.to_s
      __console_write(s.end_with?("\n") ? s : s + "\n")
    end
    nil
  end

  def p(*args)
    args.each { |a| __console_write(a.inspect + "\n") }
    args.size <= 1 ? args.first : args
  end
end
)RUBY";

mrb_value console_write(mrb_state* mrb, mrb_value /*self*/)
{
    const char* s;
    mrb_int     len;
    mrb_get_args(mrb, "s", &s, &len);
    static_cast<Console*>(mrb->ud)->write(std::string_view(s, static_cast<size_t>(len)));
    return mrb_true_value();
}

// Guess whether the user is mid-way through a construct and wants another line
// before anything runs. Ported from mirb's is_code_block_open.
bool code_block_open(const mrb_parser_state* p)
{
    if (p->parsing_heredoc) return true;
    if (p->lex_strterm) return true;
    if (p->nerr > 0) {
        static constexpr char k_unexpected_end[] = "syntax error, unexpected end of file";
        return strncmp(p->error_buffer[0].message, k_unexpected_end, sizeof(k_unexpected_end) - 1) == 0;
    }
    switch (p->lstate) {
        case EXPR_DOT:
        case EXPR_CLASS:
        case EXPR_FNAME:
        case EXPR_VALUE:
            return true;
        default:
            return false;
    }
}

mrb_value inspect_body(mrb_state* mrb, void* userdata)
{
    const mrb_value v = *static_cast<mrb_value*>(userdata);
    return mrb_inspect(mrb, v);
}

// inspect can itself raise (user-defined #inspect); never let that unwind into C++.
std::string safe_inspect(mrb_state* mrb, mrb_value v)
{
    mrb_bool        failed = false;
    const mrb_value s = mrb_protect_error(mrb, inspect_body, &v, &failed);
    if (failed || !mrb_string_p(s)) {
        mrb->exc = nullptr;
        return "#<inspect failed>";
    }
    return std::string(RSTRING_PTR(s), static_cast<size_t>(RSTRING_LEN(s)));
}

// The debug font is code page 437; anything outside printable ASCII would come
// out as box-drawing junk, so show it as '?'.
char printable(char c)
{
    if (c == '\t') return ' ';
    return (c >= 32 && c < 127) ? c : '?';
}

} // namespace

bool Console::create()
{
    m_mrb = mrb_open();
    if (!m_mrb) {
        fprintf(stderr, "console: mrb_open failed\n");
        return false;
    }
    m_mrb->ud = this;
    mrb_define_method(m_mrb, m_mrb->kernel_module, "__console_write", console_write, MRB_ARGS_REQ(1));
    mrb_load_string(m_mrb, k_prelude);
    if (m_mrb->exc) {
        fprintf(stderr, "console: prelude failed: %s\n", safe_inspect(m_mrb, mrb_obj_value(m_mrb->exc)).c_str());
        m_mrb->exc = nullptr;
    }

    m_cxt = mrb_ccontext_new(m_mrb);
    m_cxt->capture_errors = true;
    m_cxt->lineno = 1;
    mrb_ccontext_filename(m_mrb, m_cxt, "(console)");
    m_arena = mrb_gc_arena_save(m_mrb);

    m_prog  = render::load_program("vs_overlay.sc", "fs_overlay.sc");
    u_color = bgfx::createUniform("u_overlayColor", bgfx::UniformType::Vec4);
    m_layout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).end();
    if (!bgfx::isValid(m_prog))
        fprintf(stderr, "console: overlay shaders missing; drawing without a backdrop\n");

    write("mruby " MRUBY_VERSION " debug console. Type help for commands.\n", k_attr_echo);
    return true;
}

void Console::eval_startup(const char* code)
{
    mrb_load_string(m_mrb, code);
    if (m_mrb->exc) {
        write(safe_inspect(m_mrb, mrb_obj_value(m_mrb->exc)) + "\n", k_attr_error);
        m_mrb->exc = nullptr;
    }
    mrb_gc_arena_restore(m_mrb, m_arena);
}

void Console::destroy()
{
    if (bgfx::isValid(m_prog))  bgfx::destroy(m_prog);
    if (bgfx::isValid(u_color)) bgfx::destroy(u_color);
    m_prog  = BGFX_INVALID_HANDLE;
    u_color = BGFX_INVALID_HANDLE;
    if (m_cxt) mrb_ccontext_free(m_mrb, m_cxt);
    if (m_mrb) mrb_close(m_mrb);
    m_cxt = nullptr;
    m_mrb = nullptr;
}

// -----------------------------------------------------------------------
// Input
// -----------------------------------------------------------------------

void Console::set_open(bool open, SDL_Window* window)
{
    if (open == m_open) return;
    m_open = open;
    if (open) {
        // Mouselook would fight a visible cursor; hand the mouse back while typing.
        m_restore_relative_mouse = SDL_GetWindowRelativeMouseMode(window);
        SDL_SetWindowRelativeMouseMode(window, false);
        SDL_StartTextInput(window);
        m_swallow_toggle_text = true;
    } else {
        SDL_StopTextInput(window);
        SDL_SetWindowRelativeMouseMode(window, m_restore_relative_mouse);
    }
}

bool Console::handle_event(const SDL_Event& ev, SDL_Window* window)
{
    if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_GRAVE
        && (ev.key.mod & SDL_KMOD_SHIFT)) {
        if (!ev.key.repeat) set_open(!m_open, window);
        return true;
    }
    if (!m_open) return false;

    switch (ev.type) {
        case SDL_EVENT_TEXT_INPUT:
            // Whether the opening keystroke also arrives as text depends on the
            // platform's IME timing, so drop it if it does.
            if (m_swallow_toggle_text && (!strcmp(ev.text.text, "~") || !strcmp(ev.text.text, "`"))) {
                m_swallow_toggle_text = false;
                return true;
            }
            m_swallow_toggle_text = false;
            insert(ev.text.text);
            return true;
        case SDL_EVENT_KEY_DOWN:
            m_swallow_toggle_text = false;
            key_down(ev, window);
            return true;
        case SDL_EVENT_MOUSE_WHEEL:
            m_scroll += ev.wheel.y > 0 ? 3 : (ev.wheel.y < 0 ? -3 : 0);
            if (m_scroll < 0) m_scroll = 0;
            return true;
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_TEXT_EDITING:
            return true;
        default:
            return false;
    }
}

void Console::key_down(const SDL_Event& ev, SDL_Window* window)
{
    const bool ctrl = ev.key.mod & SDL_KMOD_CTRL;
    switch (ev.key.scancode) {
        case SDL_SCANCODE_ESCAPE:
            set_open(false, window);
            break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            submit();
            break;
        case SDL_SCANCODE_BACKSPACE:
            if (m_cursor > 0) m_input.erase(--m_cursor, 1);
            break;
        case SDL_SCANCODE_DELETE:
            if (m_cursor < m_input.size()) m_input.erase(m_cursor, 1);
            break;
        case SDL_SCANCODE_LEFT:
            if (m_cursor > 0) --m_cursor;
            break;
        case SDL_SCANCODE_RIGHT:
            if (m_cursor < m_input.size()) ++m_cursor;
            break;
        case SDL_SCANCODE_HOME:
            m_cursor = 0;
            break;
        case SDL_SCANCODE_END:
            m_cursor = m_input.size();
            break;
        case SDL_SCANCODE_UP:
            if (m_history_pos > 0) {
                m_input  = m_history[--m_history_pos];
                m_cursor = m_input.size();
            }
            break;
        case SDL_SCANCODE_DOWN:
            if (m_history_pos < m_history.size()) {
                ++m_history_pos;
                m_input  = m_history_pos < m_history.size() ? m_history[m_history_pos] : std::string();
                m_cursor = m_input.size();
            }
            break;
        case SDL_SCANCODE_PAGEUP:
            m_scroll += 10;
            break;
        case SDL_SCANCODE_PAGEDOWN:
            m_scroll = m_scroll > 10 ? m_scroll - 10 : 0;
            break;
        case SDL_SCANCODE_A: if (ctrl) m_cursor = 0;              break;
        case SDL_SCANCODE_E: if (ctrl) m_cursor = m_input.size(); break;
        case SDL_SCANCODE_U:
            if (ctrl) { m_input.erase(0, m_cursor); m_cursor = 0; }
            break;
        case SDL_SCANCODE_K:
            if (ctrl) m_input.erase(m_cursor);
            break;
        case SDL_SCANCODE_L:
            if (ctrl) clear();
            break;
        case SDL_SCANCODE_C:
            // Abandon the current line and any unfinished block.
            if (ctrl) {
                push_line((m_pending.empty() ? ">> " : "*> ") + m_input + "^C", k_attr_echo);
                m_input.clear();
                m_pending.clear();
                m_cursor = 0;
            }
            break;
        case SDL_SCANCODE_V:
            if (ctrl) {
                // Multi-line pastes run line by line, as if typed.
                char* clip = SDL_GetClipboardText();
                std::string_view rest = clip ? clip : "";
                for (size_t nl; (nl = rest.find('\n')) != std::string_view::npos; rest.remove_prefix(nl + 1)) {
                    insert(rest.substr(0, nl));
                    submit();
                }
                insert(rest);
                SDL_free(clip);
            }
            break;
        default:
            break;
    }
}

void Console::run_line(std::string_view line, SDL_Window* window)
{
    set_open(true, window);
    m_swallow_toggle_text = false;
    m_input.clear();
    m_cursor = 0;
    insert(line);
    submit();
}

void Console::insert(std::string_view text)
{
    std::string clean;
    for (char c : text)
        if (c != '\r') clean += printable(c);
    m_input.insert(m_cursor, clean);
    m_cursor += clean.size();
}

void Console::submit()
{
    const std::string line = m_input;
    push_line((m_pending.empty() ? ">> " : "*> ") + line, k_attr_echo);
    if (!line.empty() && (m_history.empty() || m_history.back() != line)) {
        m_history.push_back(line);
        if (m_history.size() > k_max_history) m_history.erase(m_history.begin());
    }
    m_history_pos = m_history.size();
    m_input.clear();
    m_cursor = 0;
    m_scroll = 0;

    m_pending += line;
    m_pending += '\n';
    eval(m_pending);
}

// -----------------------------------------------------------------------
// Evaluation (the mirb loop, one line at a time)
// -----------------------------------------------------------------------

void Console::eval(const std::string& code)
{
    mrb_parser_state* p = mrb_parser_new(m_mrb);
    if (!p) {
        write("parser allocation failed\n", k_attr_error);
        m_pending.clear();
        return;
    }
    p->s       = code.c_str();
    p->send    = code.c_str() + code.size();
    p->lineno  = m_cxt->lineno;
    mrb_parser_parse(p, m_cxt);

    if (code_block_open(p)) {
        // Keep m_pending; the next line is appended and the whole block reparsed.
        mrb_parser_free(p);
        m_cxt->lineno++;
        return;
    }

    if (p->nerr > 0) {
        write("line " + std::to_string(p->error_buffer[0].lineno) + ": " + p->error_buffer[0].message + "\n",
              k_attr_error);
    } else if (RProc* proc = mrb_generate_code(m_mrb, p)) {
        // The top-level env must be big enough for locals this line introduced.
        if (m_mrb->c->cibase->u.env) {
            REnv* e = mrb_vm_ci_env(m_mrb->c->cibase);
            if (e && MRB_ENV_LEN(e) < proc->body.irep->nlocals)
                MRB_ENV_SET_LEN(e, proc->body.irep->nlocals);
        }
        const mrb_value result = mrb_vm_run(m_mrb, proc, mrb_top_self(m_mrb), m_stack_keep);
        m_stack_keep = proc->body.irep->nlocals;
        flush_partial();
        if (m_mrb->exc) {
            const mrb_value exc = mrb_obj_value(m_mrb->exc);
            m_mrb->exc = nullptr;
            write(safe_inspect(m_mrb, exc) + "\n", k_attr_error);
        } else {
            write(" => " + safe_inspect(m_mrb, result) + "\n", k_attr_result);
        }
    }

    mrb_parser_free(p);
    m_cxt->lineno++;
    m_pending.clear();
    mrb_gc_arena_restore(m_mrb, m_arena);
}

// -----------------------------------------------------------------------
// Scrollback
// -----------------------------------------------------------------------

void Console::write(std::string_view text, uint8_t attr)
{
    if (!m_partial.empty() && attr != m_partial_attr) flush_partial();
    m_partial_attr = attr;
    for (char c : text) {
        if (c == '\n') {
            push_line(std::move(m_partial), attr);
            m_partial.clear();
        } else if (c != '\r') {
            m_partial += c;
        }
    }
}

void Console::flush_partial()
{
    if (m_partial.empty()) return;
    push_line(std::move(m_partial), m_partial_attr);
    m_partial.clear();
}

void Console::push_line(std::string text, uint8_t attr)
{
    for (char& c : text) c = printable(c);
    m_lines.push_back({std::move(text), attr});
    if (m_lines.size() > k_max_lines) m_lines.pop_front();
}

void Console::clear()
{
    m_lines.clear();
    m_partial.clear();
    m_scroll = 0;
}

// -----------------------------------------------------------------------
// Drawing
// -----------------------------------------------------------------------

void Console::draw(bgfx::ViewId view, uint16_t width, uint16_t height, uint16_t cols, uint16_t rows)
{
    if (!m_open) return;

    const uint16_t panel_rows = rows / 2;
    const float    panel_h    = float(panel_rows) * float(height) / float(rows);

    if (bgfx::isValid(m_prog) && bgfx::getAvailTransientVertexBuffer(6, m_layout) == 6) {
        float proj[16];
        bx::mtxOrtho(proj, 0.0f, float(width), float(height), 0.0f, 0.0f, 1.0f, 0.0f,
                     bgfx::getCaps()->homogeneousDepth);
        bgfx::setViewRect(view, 0, 0, width, height);
        bgfx::setViewTransform(view, nullptr, proj);

        bgfx::TransientVertexBuffer tvb;
        bgfx::allocTransientVertexBuffer(&tvb, 6, m_layout);
        const float w = float(width);
        const float quad[6][3] = {{0, 0, 0}, {w, 0, 0}, {w, panel_h, 0},
                                  {0, 0, 0}, {w, panel_h, 0}, {0, panel_h, 0}};
        memcpy(tvb.data, quad, sizeof(quad));
        bgfx::setVertexBuffer(0, &tvb);
        bgfx::setUniform(u_color, k_backdrop);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);
        bgfx::submit(view, m_prog);
    }

    // Wrap the scrollback to the panel width, newest at the bottom. Every row is
    // padded to full width so HUD text underneath is blanked out.
    const size_t text_w   = cols > 2 ? cols - 2 : 1;
    const int    out_rows = panel_rows - 1;   // last row is the input line
    std::vector<std::pair<std::string_view, uint8_t>> visible;
    int skip = m_scroll;
    std::vector<std::string_view> parts;
    for (auto it = m_lines.rbegin(); it != m_lines.rend() && int(visible.size()) < out_rows; ++it) {
        parts.clear();
        std::string_view t = it->text;
        do {
            parts.push_back(t.substr(0, text_w));
            t.remove_prefix(std::min(t.size(), text_w));
        } while (!t.empty());
        for (auto p = parts.rbegin(); p != parts.rend() && int(visible.size()) < out_rows; ++p) {
            if (skip > 0) { --skip; continue; }
            visible.emplace_back(*p, it->attr);
        }
    }
    if (skip > 0) m_scroll -= skip;   // clamp at the top of the history

    const std::string blank(cols, ' ');
    for (int r = 0; r < out_rows; ++r) {
        bgfx::dbgTextPrintf(0, uint16_t(r), 0x0F, "%s", blank.c_str());
        const size_t idx = size_t(out_rows - 1 - r);
        if (idx < visible.size()) {
            const auto& [txt, attr] = visible[idx];
            bgfx::dbgTextPrintf(1, uint16_t(r), attr, "%.*s", int(txt.size()), txt.data());
        }
    }

    // Input line, scrolled horizontally to keep the cursor visible.
    const uint16_t    in_row  = uint16_t(out_rows);
    const std::string prompt  = m_pending.empty() ? ">> " : "*> ";
    const size_t      field_w = text_w - prompt.size();
    const size_t      first   = m_cursor >= field_w ? m_cursor - field_w + 1 : 0;
    const std::string shown   = m_input.substr(first, field_w);
    bgfx::dbgTextPrintf(0, in_row, 0x0F, "%s", blank.c_str());
    bgfx::dbgTextPrintf(1, in_row, 0x0F, "%s%s", prompt.c_str(), shown.c_str());
    if (m_scroll > 0)
        bgfx::dbgTextPrintf(uint16_t(cols - 16), in_row, k_attr_echo, "[scrolled %4d]", m_scroll);

    if ((SDL_GetTicks() / 500) % 2 == 0) {
        const char under = m_cursor < m_input.size() ? m_input[m_cursor] : ' ';
        bgfx::dbgTextPrintf(uint16_t(1 + prompt.size() + (m_cursor - first)), in_row, 0x70, "%c", under);
    }
}

} // namespace debug
