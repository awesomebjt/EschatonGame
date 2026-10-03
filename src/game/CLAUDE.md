# src/game — Game-Specific Logic, NPCs, Items

Scope: gameplay systems layered on top of the engine core — NPC behavior, dialogue,
inventory/items, interactables. See root `CLAUDE.md` → "Component Categories" (Game
State) for the relevant components. This file is for gameplay-specific decisions: NPC
AI architecture, dialogue tree format, save-game format once chosen.

## Decisions & Notes

- **Player** (`player.*`): one EnTT entity with `PlayerTag`, `PlayerControl`,
  `CylindricalPosition`, `Velocity`, `CapsuleCollider`, `CharacterState`,
  `GravityAffected` (components in `src/ecs/components/`). `update_player` is the
  movement + physics system for it: fixed 120 Hz steps from an accumulator capped at
  0.25 s. The camera is derived from it each frame (`player_eye`).
- **Noclip** (F, `--noclip`, `Player.noclip = true`) keeps the old free-fly camera;
  leaving it drops the player in at the camera. While walking, a console edit to the
  camera (`teleport`, `Camera.x = …`) is detected in `main.cpp` and moves the player.
- **Spawn** (`find_spawn`): rings outward from the map centre for a terrain hit below
  0.5 m — street or park, never a roof, platform or bridge deck.
- **Defaults:** walk 4 m/s, sprint 8 m/s (Shift), jump 5 m/s (~1.25 m), air control
  4 m/s² only while steering. Tuning lives in `physics::CharacterParams`.
