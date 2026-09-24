# src/render — bgfx Renderer, Instancing, LOD, Backdrop

Scope: bgfx setup, per-type instanced building draws, chunk terrain draw, LOD
swapping, the fog ramp, and the backdrop shell (baked plan-view mesh with parallax
occlusion mapping, re-rendered only on lighting change). See root `CLAUDE.md` →
"Rendering" (Backdrop Shell, Level of Detail Policy, Draw Call Budget) and →
"Terrain System" → "GPU Curvature Strategy" for the design intent. This file is for
bgfx-specific implementation notes: backend quirks, shader compile flags, resource
lifetime/threading rules, `BGFX_CAPS_INSTANCING` fallback handling.

## Decisions & Notes

_(none yet — add short entries here as design choices are made: what was decided, why,
and what alternatives were rejected)_
