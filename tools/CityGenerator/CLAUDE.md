# tools/CityGenerator — Procedural World Generator

Scope: `paris_city.py` and the export/preview/bake scripts that turn a seed into
engine-ready chunk data (Python). See root `CLAUDE.md` → "World Generation" for the
current pipeline (river → hubs → boulevards → superblocks → minor streets →
buildings → parks, Haussmann-style) and → "World Data Formats" / `docs/WORLD_PIPELINE.md`
for what gets exported. Lakes, industrial/science districts, agriculture, and density
variation are documented there as planned-but-not-yet-implemented passes.

This file is for implementation notes specific to this tool: library choices,
performance tricks at map scale, deviations from the pipeline spec and why.

## Decisions & Notes

- 2026-09-24: superseded the earlier 256×204 biome-grid / Poisson-disk lake-and-zone
  generator design in favor of the Haussmann-city generator now documented in root
  `CLAUDE.md`. The biome-grid approach is retained as the planned mechanism for the
  not-yet-implemented lake/industrial/agricultural passes (as a mask feeding the
  generator *before* the hub pass), not as the primary pipeline.
