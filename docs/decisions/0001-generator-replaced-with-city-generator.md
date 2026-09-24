# 0001: World generator replaced with Haussmann-style city generator; terrain moved from authored tiles to generated chunks

Date: 2026-09-24
Status: decided

## Decision

Replaced the 256×204 biome-grid generator (Poisson-disk lakes, noise-partitioned
urban/industrial fill) with a deterministic Haussmann-Paris-style street/building
generator (`tools/CityGenerator/paris_city.py`). Terrain authoring moved from a
library of hand-mated 500 m tiles (TOML index + CSV layout grid) to 502×500 m chunks
produced directly by the generator and exported as binary mesh + instance data
(`docs/WORLD_PIPELINE.md`). Also updated canonical cylinder dimensions: circumference
25,100 m / radius 3,994.8 m (was 25,132.7 m / 4,000 m), so that 50 chunk columns of
502 m close the circumference exactly.

## Why

The city's road network is continuous and doesn't decompose into a small set of
mating tiles the way parkland/lakeshore/industrial biomes did. Chunk-exported mesh
data plus building instance tables (rather than 375k individual meshes/entities)
keeps memory and draw-call budgets sane. The circumference change is a rounding
choice to make chunk-column wrap arithmetic exact, at a cost of 0.05% in ω — invisible
in play.

## Alternatives considered

- Keeping the biome-grid generator and layering the city on top: rejected, the two
  don't compose cleanly since the biome grid was designed around discrete tile types,
  not a continuous street graph.
- Keeping the old 25,132.7 m circumference and accepting non-integer chunk columns:
  rejected, breaks the `mod 50` wrap identity the streaming/rebase system depends on.

The old biome-grid approach isn't discarded — it's retained as the planned mechanism
for lakes/industrial/agricultural passes (as a mask feeding the generator before the
hub pass), per root `CLAUDE.md` → "World Generation" → "Not Yet Generated".
