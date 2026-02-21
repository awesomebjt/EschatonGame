# Terrain Tile TODO

This file catalogs every tile type needed for the Eschaton terrain system. Each tile is a 500m × 500m ground section authored in Blender.

**Authoring pipeline for each tile:**

1. Create as a flat textured quad (top-down view) — used for skybox texture and lowest LOD.
2. Extrude to 3–5m height with appropriate surface detail — used as the player's walkable terrain.
3. Export as glTF for engine consumption.

Tiles are placed on a grid and curved onto the cylinder surface by the vertex shader at runtime. Each tile covers a 4×4 block of the 256×204 biome generation grid.

Rotation variants (0°/90°/180°/270°) are handled by the engine at placement time and do NOT require separate tile assets. Tiles marked **(rotatable)** are asymmetric and gain coverage from rotation. Tiles marked **(symmetric)** look the same under rotation.

---

## Naming Convention

`{biome}_{variant}` for pure tiles, `{biomeA}_{biomeB}_{pattern}` for transitions.

Edge direction convention (looking top-down, before any placement rotation):
- **N/S** = Z-axis (along cylinder length)
- **E/W** = θ-axis (around circumference)

---

## 1. Pure Biome Tiles

These tiles are entirely one biome. Multiple visual variants of each prevent repetition.

### Water

| # | Tile ID | Description | Status |
|---|---------|-------------|--------|
| 1 | `water_deep_01` | Deep open water, no visible bottom (symmetric) | ☐ |
| 2 | `water_deep_02` | Deep open water variant, subtle surface variation (symmetric) | ☐ |
| 3 | `water_shallow_01` | Shallow water, visible lakebed (symmetric) | ☐ |

### Park / Green Space

| # | Tile ID | Description | Status |
|---|---------|-------------|--------|
| 4 | `park_open_01` | Open grass with gentle rolling terrain (symmetric) | ☐ |
| 5 | `park_open_02` | Open grass variant, slightly different contour (symmetric) | ☐ |
| 6 | `park_garden_01` | Formal gardens, hedgerows, paths (symmetric) | ☐ |
| 7 | `park_path_ns` | Park with footpath running N–S (rotatable) | ☐ |
| 8 | `park_path_cross` | Park with crossing footpaths (symmetric) | ☐ |

### Forest

| # | Tile ID | Description | Status |
|---|---------|-------------|--------|
| 9 | `forest_dense_01` | Dense canopy, minimal ground detail visible (symmetric) | ☐ |
| 10 | `forest_dense_02` | Dense canopy variant (symmetric) | ☐ |
| 11 | `forest_clearing_01` | Forest with small clearing (symmetric) | ☐ |
| 12 | `forest_path_ns` | Forest with trail running N–S (rotatable) | ☐ |

### Urban / Residential

| # | Tile ID | Description | Status |
|---|---------|-------------|--------|
| 13 | `urban_residential_01` | Low-rise residential blocks with streets (symmetric) | ☐ |
| 14 | `urban_residential_02` | Residential variant, different block layout (symmetric) | ☐ |
| 15 | `urban_commercial_01` | Mixed commercial, shops, wider streets (symmetric) | ☐ |
| 16 | `urban_downtown_01` | Dense downtown, tall building foundations (symmetric) | ☐ |
| 17 | `urban_road_ns` | Major road corridor running N–S (rotatable) | ☐ |
| 18 | `urban_road_cross` | Major intersection / crossroads (symmetric) | ☐ |
| 19 | `urban_plaza_01` | Open civic plaza / town square (symmetric) | ☐ |

### Industrial / Science

| # | Tile ID | Description | Status |
|---|---------|-------------|--------|
| 20 | `industrial_heavy_01` | Large factory foundations, pipe runs (symmetric) | ☐ |
| 21 | `industrial_heavy_02` | Heavy industry variant (symmetric) | ☐ |
| 22 | `industrial_light_01` | Light manufacturing, warehouses (symmetric) | ☐ |
| 23 | `industrial_science_01` | Research campus, lab buildings (symmetric) | ☐ |
| 24 | `industrial_yard_01` | Storage yards, container areas (symmetric) | ☐ |
| 25 | `industrial_road_ns` | Industrial service road running N–S (rotatable) | ☐ |

---

## 2. Biome Transition Tiles — Straight Edge

One biome occupies roughly half the tile, the other biome the other half, divided by a straight or gently curved boundary running across the tile. The boundary runs **E–W** in the canonical orientation; rotation provides N–S variants.

**Allowed adjacencies** (industrial cannot touch water directly):

| # | Tile ID | Description | Status |
|---|---------|-------------|--------|
| 26 | `water_park_edge_ew` | Water (N half) meets park (S half), gentle shoreline (rotatable) | ☐ |
| 27 | `water_park_edge_ew_02` | Shoreline variant, rockier bank (rotatable) | ☐ |
| 28 | `water_urban_edge_ew` | Water (N half) meets urban (S half), seawall / promenade (rotatable) | ☐ |
| 29 | `water_forest_edge_ew` | Water (N half) meets forest (S half), wooded shoreline (rotatable) | ☐ |
| 30 | `park_urban_edge_ew` | Park (N half) transitions to urban (S half) (rotatable) | ☐ |
| 31 | `park_forest_edge_ew` | Park (N half) transitions to forest (S half), tree line (rotatable) | ☐ |
| 32 | `park_industrial_edge_ew` | Park (N half) meets industrial (S half), fence line (rotatable) | ☐ |
| 33 | `urban_industrial_edge_ew` | Urban (N half) transitions to industrial (S half) (rotatable) | ☐ |
| 34 | `urban_forest_edge_ew` | Urban (N half) meets forest (S half), city edge (rotatable) | ☐ |
| 35 | `forest_industrial_edge_ew` | Forest (N half) meets industrial (S half) (rotatable) | ☐ |

---

## 3. Biome Transition Tiles — Corner

One biome occupies a corner quadrant (NE, NW, SE, or SW), the other fills the rest. Canonical orientation has the minority biome in the **NE corner**; rotation provides all four corner positions.

| # | Tile ID | Description | Status |
|---|---------|-------------|--------|
| 36 | `water_park_corner_ne` | Water in NE corner, park elsewhere — cove / inlet (rotatable) | ☐ |
| 37 | `water_urban_corner_ne` | Water in NE corner, urban elsewhere — harbor corner (rotatable) | ☐ |
| 38 | `water_forest_corner_ne` | Water in NE corner, forest elsewhere (rotatable) | ☐ |
| 39 | `park_urban_corner_ne` | Park in NE corner, urban elsewhere (rotatable) | ☐ |
| 40 | `park_industrial_corner_ne` | Park in NE corner, industrial elsewhere (rotatable) | ☐ |
| 41 | `park_forest_corner_ne` | Park in NE corner, forest elsewhere (rotatable) | ☐ |
| 42 | `urban_industrial_corner_ne` | Urban in NE corner, industrial elsewhere (rotatable) | ☐ |
| 43 | `urban_forest_corner_ne` | Urban in NE corner, forest elsewhere (rotatable) | ☐ |
| 44 | `forest_industrial_corner_ne` | Forest in NE corner, industrial elsewhere (rotatable) | ☐ |

---

## 4. Biome Transition Tiles — Inverse Corner (Peninsula/Bulge)

One biome fills most of the tile, with the other biome filling three quadrants and wrapping around a corner — the inverse of category 3. Canonical: majority biome in **NE corner only**, minority biome wraps the other three quadrants. This creates a peninsula or bulge of the majority biome.

| # | Tile ID | Description | Status |
|---|---------|-------------|--------|
| 45 | `park_water_peninsula_ne` | Park peninsula into water (rotatable) | ☐ |
| 46 | `urban_water_peninsula_ne` | Urban headland into water, pier/dock feel (rotatable) | ☐ |
| 47 | `forest_water_peninsula_ne` | Forested point into water (rotatable) | ☐ |
| 48 | `urban_park_peninsula_ne` | Urban block extending into park (rotatable) | ☐ |
| 49 | `industrial_urban_peninsula_ne` | Industrial zone extending into urban (rotatable) | ☐ |
| 50 | `forest_park_peninsula_ne` | Forest extending into park (rotatable) | ☐ |
| 51 | `industrial_park_peninsula_ne` | Industrial extending into park (rotatable) | ☐ |
| 52 | `forest_urban_peninsula_ne` | Forest extending into urban (rotatable) | ☐ |
| 53 | `industrial_forest_peninsula_ne` | Industrial extending into forest (rotatable) | ☐ |

---

## 5. Biome Transition Tiles — Diagonal

Tile is split diagonally. One biome in the NE triangle, the other in the SW triangle. Rotation provides the NW/SE split.

| # | Tile ID | Description | Status |
|---|---------|-------------|--------|
| 54 | `water_park_diag_nesw` | Diagonal shoreline, water NE, park SW (rotatable) | ☐ |
| 55 | `water_urban_diag_nesw` | Diagonal shoreline, water NE, urban SW (rotatable) | ☐ |
| 56 | `water_forest_diag_nesw` | Diagonal shoreline, water NE, forest SW (rotatable) | ☐ |
| 57 | `park_urban_diag_nesw` | Park NE, urban SW (rotatable) | ☐ |
| 58 | `park_industrial_diag_nesw` | Park NE, industrial SW (rotatable) | ☐ |
| 59 | `park_forest_diag_nesw` | Park NE, forest SW (rotatable) | ☐ |
| 60 | `urban_industrial_diag_nesw` | Urban NE, industrial SW (rotatable) | ☐ |
| 61 | `urban_forest_diag_nesw` | Urban NE, forest SW (rotatable) | ☐ |
| 62 | `forest_industrial_diag_nesw` | Forest NE, industrial SW (rotatable) | ☐ |

---

## 6. River Tiles

Rivers are 1 cell wide (~125m) on the biome map, but occupy a full 500m tile. The river channel itself is ~100–150m wide with banks filling the rest. Each river tile has a biome context for the surrounding land.

### River — Straight

| # | Tile ID | Description | Status |
|---|---------|-------------|--------|
| 63 | `river_straight_park_ns` | Straight river running N–S through park (rotatable) | ☐ |
| 64 | `river_straight_urban_ns` | Straight river running N–S through urban, canal walls (rotatable) | ☐ |
| 65 | `river_straight_forest_ns` | Straight river running N–S through forest (rotatable) | ☐ |

### River — Gentle Bend

Curves approximately 45°. Canonical: enters from S, exits toward NE.

| # | Tile ID | Description | Status |
|---|---------|-------------|--------|
| 66 | `river_bend_park_s_ne` | Gentle river bend in park (rotatable) | ☐ |
| 67 | `river_bend_urban_s_ne` | Gentle river bend in urban, canal curve (rotatable) | ☐ |
| 68 | `river_bend_forest_s_ne` | Gentle river bend in forest (rotatable) | ☐ |

### River — Sharp Bend (90°)

Enters from S, exits toward E.

| # | Tile ID | Description | Status |
|---|---------|-------------|--------|
| 69 | `river_sharp_park_s_e` | 90° river bend in park (rotatable) | ☐ |
| 70 | `river_sharp_urban_s_e` | 90° river bend in urban (rotatable) | ☐ |
| 71 | `river_sharp_forest_s_e` | 90° river bend in forest (rotatable) | ☐ |

### River — Confluence / Fork

Two rivers merge into one. Canonical: two channels enter from S and E, one exits N.

| # | Tile ID | Description | Status |
|---|---------|-------------|--------|
| 72 | `river_confluence_park` | River fork/merge in park (rotatable) | ☐ |
| 73 | `river_confluence_urban` | River fork/merge in urban (rotatable) | ☐ |

### River — Lake Mouth

River meets a body of water. Canonical: river enters from S, lake on N half.

| # | Tile ID | Description | Status |
|---|---------|-------------|--------|
| 74 | `river_mouth_park_s` | River emptying into lake, park banks (rotatable) | ☐ |
| 75 | `river_mouth_urban_s` | River emptying into lake, urban banks / harbor (rotatable) | ☐ |
| 76 | `river_mouth_forest_s` | River emptying into lake, forested banks (rotatable) | ☐ |

### River — Biome Transition

River runs along the boundary between two biomes.

| # | Tile ID | Description | Status |
|---|---------|-------------|--------|
| 77 | `river_border_park_urban_ns` | River running N–S, park on W bank, urban on E bank (rotatable) | ☐ |
| 78 | `river_border_park_forest_ns` | River running N–S, park on W bank, forest on E bank (rotatable) | ☐ |
| 79 | `river_border_park_industrial_ns` | River running N–S, park on W bank, industrial on E bank (rotatable) | ☐ |
| 80 | `river_border_urban_forest_ns` | River running N–S, urban on W bank, forest on E bank (rotatable) | ☐ |

---

## 7. Three-Way Transition Tiles

Where three biomes meet at one tile. These are less common but necessary for natural-looking boundaries.

| # | Tile ID | Description | Status |
|---|---------|-------------|--------|
| 81 | `water_park_urban_t` | Water (N), park (SW), urban (SE) — waterfront park next to city (rotatable) | ☐ |
| 82 | `water_park_forest_t` | Water (N), park (SW), forest (SE) (rotatable) | ☐ |
| 83 | `park_urban_industrial_t` | Park (N), urban (SW), industrial (SE) (rotatable) | ☐ |
| 84 | `park_urban_forest_t` | Park (N), urban (SW), forest (SE) (rotatable) | ☐ |
| 85 | `park_forest_industrial_t` | Park (N), forest (SW), industrial (SE) (rotatable) | ☐ |
| 86 | `urban_forest_industrial_t` | Urban (N), forest (SW), industrial (SE) (rotatable) | ☐ |

---

## 8. Special / Landmark Tiles

Unique tiles for specific features that don't fit the biome system.

| # | Tile ID | Description | Status |
|---|---------|-------------|--------|
| 87 | `column_base_park` | Central light column support structure base, park surroundings | ☐ |
| 88 | `column_base_urban` | Central light column support structure base, urban surroundings | ☐ |
| 89 | `column_base_industrial` | Central light column support structure base, industrial surroundings | ☐ |
| 90 | `dam_ns` | Dam / weir structure across a river, N–S orientation (rotatable) | ☐ |
| 91 | `bridge_park_ns` | Bridge crossing river in park context (rotatable) | ☐ |
| 92 | `bridge_urban_ns` | Bridge crossing river in urban context (rotatable) | ☐ |
| 93 | `transit_station_urban` | Major transit hub foundation (symmetric) | ☐ |
| 94 | `transit_station_industrial` | Industrial transit stop (symmetric) | ☐ |
| 95 | `endcap_park` | Cylinder endcap transition — ground curving to meet the end wall, park (rotatable) | ☐ |
| 96 | `endcap_urban` | Cylinder endcap transition, urban (rotatable) | ☐ |
| 97 | `endcap_industrial` | Cylinder endcap transition, industrial (rotatable) | ☐ |
| 98 | `endcap_forest` | Cylinder endcap transition, forest (rotatable) | ☐ |
| 99 | `island_small` | Small island in a lake (symmetric) | ☐ |
| 100 | `island_large_ne` | Large island corner piece (rotatable) | ☐ |

---

## Summary

| Category | Count |
|----------|-------|
| Pure biome tiles | 25 |
| Straight edge transitions | 10 |
| Corner transitions | 9 |
| Inverse corner (peninsula) transitions | 9 |
| Diagonal transitions | 9 |
| River tiles | 18 |
| Three-way transitions | 6 |
| Special / landmark tiles | 14 |
| **Total unique tile assets** | **100** |

With rotation variants (×4 for asymmetric tiles), effective tile coverage is significantly higher than 100 without additional modeling work.

---

## Notes for Blender Authoring

- All tiles are authored as **flat 500m × 500m** quads. The cylinder curvature is applied by the vertex shader at runtime.
- Each tile should have a consistent edge elevation (0m baseline) so tiles mate cleanly.
- Internal terrain variation (hills, depressions, banks) should stay within 0–5m height range for the extruded version.
- UV mapping should use a consistent world-space scale so textures don't visibly change density at tile boundaries.
- River channel cross-sections should be consistent across all river tiles: ~100–150m wide, ~3–5m depressed from surrounding terrain.
- The flat textured quad version of each tile doubles as the skybox texture source and the lowest LOD tier. Color palette and contrast should read clearly at extreme distance.
