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

- **Everything loads at once** (`world_loader.cpp`): all 3,200 chunks, worker threads
  per terrain group, ~0.2 s on 16 cores. No streaming/eviction yet.
- **Terrain subdivision happens at load, not in the exporter or on the GPU.** Each
  exported face is regrouped from its fan and clipped at map-X lines every
  `chunk_x / ceil(chunk_x / 25)` ≈ 23.9 m, measured from a chunk edge. Because the lines
  are global, neighbouring faces split at identical points (no T-junction cracks) and
  stacked layers (ground / water / park / road) stay parallel within every strip, so
  chord sag can't reorder them. Only X is cut: the cylinder is straight along Y.
  1.7M → 6.2M triangles. Exporter-side cutting with the same rule would be equivalent.
- **Monument instance tables are not drawn**: the monuments are already baked into the
  terrain mesh by `build_chunk_meshes`. The tables are counted and kept for scripting.
- **Known data bug (generator):** parks stop 2 m short of every chunk's +X edge
  (`step = int(CHUNK_X / PARK_CELL)` truncates 200.8 cells to 200 in
  `build_chunk_meshes`), leaving a ground-coloured seam through parks every 502 m.
