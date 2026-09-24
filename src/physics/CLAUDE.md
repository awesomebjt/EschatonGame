# src/physics — Rotating-Frame Physics & Collision

Scope: centrifugal/Coriolis integration, physics LOD tiering, collision. See root
`CLAUDE.md` → "Physics" for the governing formulas (ω = sqrt(g/r) ≈ 0.049548 rad/s at
r = 3994.8 m, Coriolis = -2ω×v), the LOD distance table, and → "Collision" — the
world exporter emits no collision meshes, so buildings/ground/bridges/water colliders
are derived at load time from `manifest.json` footprints and fixed assumptions (flat
ground, ramped bridge decks, un-carved flat river). This file is for
implementation-level decisions: integrator choice details, numerical stability
tricks, tuning constants that deviate from the reference formulas and why.

## Decisions & Notes

_(none yet — add short entries here as design choices are made: what was decided, why,
and what alternatives were rejected)_
