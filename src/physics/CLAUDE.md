# src/physics — Rotating-Frame Physics & Collision

Scope: centrifugal/Coriolis integration, physics LOD tiering, collision. See root
`CLAUDE.md` → "Physics" for the governing formulas (ω = sqrt(g/r) ≈ 0.049548 rad/s at
r = 3994.8 m, Coriolis = -2ω×v), the LOD distance table, and → "Collision" — the
world exporter emits no collision meshes, so buildings/ground/bridges/water colliders
are derived at load time from the exported terrain and prototype meshes (see the
notes below; the river is a flat solid surface for now). This file is for
implementation-level decisions: integrator choice details, numerical stability
tricks, tuning constants that deviate from the reference formulas and why.

## Decisions & Notes

- **Collision is flat map space, not the cylinder.** `CollisionWorld` keeps the
  exporter's terrain triangles (chunk-relative floats) and building instances, bucketed
  into a ~32 m grid that wraps in x. Queries return triangles relative to a double-
  precision origin, so all contact maths is small floats. Buildings use their LOD0
  prototype mesh (passages stay open), not footprint boxes.
- **Character controller is kinematic, not a rigid body** (`character.cpp`): velocity
  from input + pseudoforces, then move-and-slide in three passes — lift by
  `step_height`, horizontal in half-radius sub-steps, then down with a ground snap.
  Depenetration is discrete (deepest contact first, 6 rounds); sub-stepping is what
  stops tunnelling, so keep moves per pass under half the radius.
- **Walkable contacts push straight up** (n.z ≥ cos 45°) so standing on a ramp doesn't
  creep downhill; steep contacts push along the normal and clip velocity into them.
- **No air braking without input.** Air control only acts while steering: braking toward
  a zero wish (4 m/s²) almost exactly cancelled the Coriolis drift of a fall.
- **Map x vs local metres:** velocity is local metres; horizontal x displacement is
  scaled by R / (R − alt) into map metres before colliding (1.0 at the floor).
- **Spin direction** is +x (see root CLAUDE.md); `RotatingFrame::spin = −1` for the
  counter-rotating cylinder. Measured with `physics_check`: a 100 m drop drifts within
  ~4% of the small-height estimate (2/3)·ω·sqrt(2h³/g), anti-spinward.
- **Cost:** ~85 µs per 120 Hz step for the player (≈ 0.2 ms per frame at 60 fps).
  `physics_check` fails above 500 µs.
- **Not covered by `physics_check` yet:** bridge ramps and roundabout platforms (the
  monument table isn't kept in `WorldData`). Platforms are 1.5 m: above the 0.35 m
  step and the ~1.25 m jump, so they're unreachable by design for now.
