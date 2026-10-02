# src/input — SDL3 Input Polling & Action Mapping

Scope: raw SDL3 input polling, the abstract `Action` enum mapping layer, remappable
bindings config. See root `CLAUDE.md` → "Input Abstraction" for the intent. This file
is for implementation notes: SDL3 event quirks, gamepad deadzone/curve tuning,
binding-file schema decisions.

## Decisions & Notes

- **Mouselook needs `SDL_SetWindowRelativeMouseMode`, not `SDL_SetWindowMouseGrab`.**
  A grab only confines the pointer; once it pins against the window edge (immediately
  on Wayland) `xrel` stops and yaw stalls. Relative mode hides the cursor and delivers
  unbounded deltas. Mouselook is still in `main.cpp` until this module exists.
