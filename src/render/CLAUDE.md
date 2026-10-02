# src/render — bgfx Renderer, Instancing, LOD, Backdrop

Scope: bgfx setup, per-type instanced building draws, chunk terrain draw, LOD
swapping, the fog ramp, and the backdrop shell (baked plan-view mesh with parallax
occlusion mapping, re-rendered only on lighting change). See root `CLAUDE.md` →
"Rendering" (Backdrop Shell, Level of Detail Policy, Draw Call Budget) and →
"Terrain System" → "GPU Curvature Strategy" for the design intent. This file is for
bgfx-specific implementation notes: backend quirks, shader compile flags, resource
lifetime/threading rules.

## Decisions & Notes

- **Camera always at θ = 0.** Render space is camera-relative: +X tangent, +Y toward
  the axis, +Z along the axis (map +Y). `shaders/cylinder.sh` wraps map-space offsets
  onto the cylinder around the camera every frame, so circumferential movement never
  produces large coordinates and the X wrap is implicit (sin/cos are periodic). There
  is no explicit rebase step yet; the view matrix is rotation-only, eye at the origin.
- **Whole-cylinder draw, ~181 calls.** Terrain is merged into 160 groups of 5 × 4 chunks
  (vertices relative to the group centre, offset per draw via `u_offset`), plus one
  emissive light-column tube. Buildings are **one instanced draw per type for all
  272k instances**: the instance carries chunk-relative position + chunk (col, row), and
  the shader rebuilds a camera-relative origin from whole-chunk steps, so no absolute
  25 km coordinate is ever held in float. This replaces the per-chunk band budget in the
  root file for now; no frustum culling, no LOD (prototypes are 10–46 tris, all LOD0).
- **Buildings are rigid** (tangent plane at their origin, per root CLAUDE.md); the base
  ring is sunk by x² / 2R + 0.1 m so wide footprints meet the curving floor.
- **Reversed-Z, infinite far, D32F** on [0, 1]-depth backends. Terrain layers sit 5 cm
  apart and are seen from 8 km; standard depth z-fights from ~1 km. OpenGL
  (`homogeneousDepth`) falls back to a normal projection with near = 2 m.
- **Haze, not the 4–7 km fog wall.** Fog is exponential from 1.5 km, rescaled to be
  fully opaque at 7 km (`k_fog_*` in `world_renderer.cpp`), so the far side is hidden
  until the backdrop shell exists. The light column is only 30% fogged so it stays
  visible end to end.
- **`BGFX_CAPS_INSTANCING` no longer exists.** bgfx raised its renderer baseline
  (upstream 8c8b6b569) and removed caps every backend supports; no fallback needed.
- **`--shot file.png [--pos x y alt] [--view yaw pitch]`** renders one frame and exits,
  for visual checks without driving the menu. PNGs come from the bgfx callback in
  `main.cpp`.
