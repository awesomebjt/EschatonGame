# src/world — Chunk Loading, Origin Rebasing, Transitions

Scope: chunk streaming (`chunk_III_JJJ.bin` load/evict), the wrap/rebase math, and
cylinder transition + persistent-state snapshotting. See root `CLAUDE.md` →
"Coordinate Systems" (map / cylindrical / camera-relative spaces, and the `wrap_dx`/
`wrap_x` helpers — X wraps at 25,100 m, chunk columns wrap `mod 50`), "Terrain
System" → "Chunk Streaming", and "Cylinder Transition & Persistent State" for the
design. This file is for implementation notes: snapshot format versioning, chunk
worker-thread/render-thread split, rebase trigger tuning (fires on 502 m column
crossing — remember the backdrop shell rotates on rebase, it does not translate).

## Decisions & Notes

_(none yet — add short entries here as design choices are made: what was decided, why,
and what alternatives were rejected)_
