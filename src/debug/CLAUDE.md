# src/debug — In-Game Ruby Console

Scope: the Shift+` drop-down console (`console.*`) and the Ruby commands it exposes
(`bindings.*`). Developer tooling only; nothing in the game proper may depend on it.

## Decisions & Notes

- **mruby 3.4.0, built by its own rake, not CMake.** `CMakeLists.txt` fetches the
  source and runs `rake` with `MRUBY_CONFIG=cmake/mruby_config.rb`, output in
  `build*/mruby/host/`. Needs `ruby` and `rake` on the build machine. Generated presym
  headers live in `host/include`, which is why the imported target adds that dir.
  No IO gems: output goes to the scrollback, and there is no file/socket access.
- **Evaluation is mirb's loop** (`mruby-bin-mirb/tools/mirb/mirb.c`): one persistent
  `mrb_ccontext`, `mrb_vm_run` with `stack_keep` so locals survive between lines, and
  `code_block_open` (ported from mirb) to continue unfinished blocks on the next line.
  Re-check it against mirb when bumping mruby; it touches parser internals.
- **print/puts/p are redefined in a Ruby prelude** to call `__console_write`; mruby's
  built-ins write to stdout.
- **No C++ object with a destructor may be live across `mrb_get_args`/`mrb_raise`.**
  mruby unwinds with longjmp. `safe_inspect` uses `mrb_protect_error` because a
  user-defined `#inspect` can raise.
- **Drawing:** backdrop is one alpha-blended quad on view 1 (`vs_overlay`/`fs_overlay`);
  text is bgfx debug text, every row padded to full width to blank the HUD underneath.
  The debug font is CP437, so non-ASCII is shown as `?`.
- **Input:** the console sees every SDL event first and swallows keyboard/mouse while
  open; it turns relative mouse mode off and restores it on close. `~` cannot be typed
  (it is the toggle).
- **`--console "ruby"`** (repeatable) opens the console after the world loads and runs
  the line as if typed; with `--shot` it is the headless way to test commands.
- **Adding a command:** C functions in `bindings.cpp` reach game state through
  `DebugHost` (pointers into `main.cpp` globals); thin Ruby sugar goes in
  `k_ruby_helpers`, and `help` should list it.

## Known Limitations

- `~` can't be typed (it is the toggle); non-ASCII displays as `?`.
- No file or socket access (IO gems left out); no `load`/`require` of scripts.
- No timeout: an infinite loop typed into the console hangs the game.
- `screenshot` captures the console too, if it is open.
- Evaluation and drawing were verified via `--console` + `--shot`; the live keyboard
  path (toggle, editing, history, paste) had no automated check when it landed.
