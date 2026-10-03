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
  apart and are seen from 8 km; standard depth z-fights from ~1 km. Near plane is
  0.05 m because the eye can be a capsule radius (0.3 m) from a wall. OpenGL
  (`homogeneousDepth`) falls back to a normal projection with near = 0.1 m and
  accepts distant z-fighting.
- **Haze, not the 4–7 km fog wall.** Fog is exponential from 1.5 km, rescaled to be
  fully opaque at 7 km (`FogParams` in `world_renderer.h`, live-tunable via `WorldRenderer::fog`), so the far side is hidden
  until the backdrop shell exists. The light column is only 30% fogged so it stays
  visible end to end.
- **`BGFX_CAPS_INSTANCING` no longer exists.** bgfx raised its renderer baseline
  (upstream 8c8b6b569) and removed caps every backend supports; no fallback needed.
- **`--shot file.png [--pos x y alt] [--view yaw pitch] [--noclip] [--console ruby]... [--exec ruby]...`** renders one frame and exits,
  for visual checks without driving the menu; the mouse is ignored. `--exec` runs a
  console line without showing the console. PNGs come from the bgfx callback in
  `main.cpp`.
- **Clouds are sorted sphere-impostor puffs** (`cloud_renderer.*`, `vs_cloud`/`fs_cloud`):
  ~370 cumulus × ~14 puffs from `world/clouds.*` (seeded, wraps in x and y). Each frame
  the CPU builds camera-relative instances, culls past the opaque fog, sorts back to
  front and submits one instanced draw; blended, so bgfx sorts it after the opaque
  world in view 0. The fragment shader ray-traces each puff as a sphere sliced flat at
  the cloud base: a ray meeting the sphere below the base may still enter through the
  base and then sees the flat underside. (Merely discarding below-base fragments, the
  first version, showed rings and crescents from underneath: the near cap is cut away
  and an impostor has nothing behind it.) Downward faces all get the same ambient
  shade so bases and puff undersides meet without seams; the base softens only when
  seen edge-on. Ray-marched volumetrics remain an option later and can reuse the same
  seeded field.
- **Puff offsets are true metres at the cloud's radius**, so map x is stretched by
  R / (R − alt) (1.6× at 1.5 km), or clouds would look 38% too narrow around.
- **Cloud shadow map:** R8, 512 × 653 (~49 m/texel) over the whole floor, built once per
  field and sampled by map position in `fs_world` (terrain and buildings). Light is
  radial, so a puff's shadow is its outline stretched 1.6× around the circumference;
  the line light smears it along the axis with the kernel (2/π)/(1 + u²)², u = axial
  offset / height. Physically weak: ~40% of the column's light at most, applied only to
  the direct term, so ~13–24% darker. `Clouds.shadow` scales it (default 1 = physical).
  Wind drift is an offset into the map, never a rebuild.
- **Wind defaults to 5 m/s spinward** and towers lean spinward (`CloudParams::lean`):
  rising air keeps the floor's angular momentum, which exceeds the frame's at smaller
  radius, so the habitat's analogue of a Hadley cell gives spinward winds aloft and an
  anti-spinward surface breeze (not modelled yet). Horizontal winds are never deflected
  sideways (ω ∥ axis), so there are no cyclones; axial winds run straight.
- **`clouds_check`** (target) checks floor coverage (target 12%, measured 13.4%) and the
  peak shadow, and can dump the shadow map as a PGM.
