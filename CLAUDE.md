# Eschaton

## Project Overview

Eschaton is a first-person 3D game set inside a pair of counter-rotating O'Neill cylinders in deep space. The player explores two massive enclosed habitats — cylindrical worlds with cities, parks, lakes, and rivers built along the inner wall. The game is built in C++ using bgfx for cross-platform rendering, SDL3 for windowing/input, and EnTT for Entity Component System architecture.

The project is code-first with no visual editor. All scene composition, entity placement, and asset pipelines are defined in code or declarative data files.

---

## Technology Stack

| Layer | Technology | Purpose |
|---|---|---|
| Language | C++20 | Core language |
| Rendering | bgfx | Cross-platform graphics abstraction (Vulkan, Metal, DX11/12, OpenGL) |
| Shaders | bgfx shaderc | Write once, compile to GLSL/HLSL/Metal/SPIR-V |
| Windowing & Input | SDL3 | Platform abstraction, input handling, gamepad support |
| ECS | EnTT | Entity Component System |
| Build | CMake | Cross-platform build system |
| Physics | Custom | Rotating reference frame physics with Coriolis correction |
| Audio | TBD | (OpenAL Soft, FMOD, or miniaudio — decide later) |
| Asset Formats | TBD | (glTF likely for meshes, custom for world data) |

### Why These Choices

- **bgfx** provides shader portability across all major graphics APIs without writing backend-specific code. Shaders are written in a GLSL-like dialect and compiled via `shaderc` to every target.
- **SDL3** is the latest major version of SDL, handling windowing, input abstraction (keyboard, mouse, gamepad), and platform differences.
- **EnTT** is a header-only, cache-friendly ECS library that is mature, fast, and well-documented. It will be the backbone of all game object management.
- **C++20** for structured bindings, concepts, `std::span`, and other ergonomic improvements. Avoid bleeding-edge C++23 features that may reduce compiler portability.

---

## World Architecture

### The Cylinders

The game world consists of two O'Neill cylinders. Only one cylinder is loaded into memory at a time; the other is the second "level."

| Property | Value |
|---|---|
| Length | 32,000 m (32 km) |
| Diameter | 8,000 m (8 km) |
| Radius | 4,000 m |
| Hull thickness | ~10 m |
| Interior surface area | ~804 km² per cylinder |
| Rotation | Tuned to produce ~1g at the inner wall |

The cylinders are **fully enclosed** — there is no sky, no windows. The interior is lit by a **central luminous column** running the full 32 km length of the cylinder, suspended along the rotational axis. This column provides the day/night cycle and all ambient light.

The "floor" is the **inner wall** of the cylinder. Gravity points radially outward from the axis of rotation. The player walks on the concave interior surface. Looking up, the landscape curves away and eventually arcs overhead.

### Interior Composition

Each cylinder's interior is a mix of:

- **Urban zones** — cities and towns with buildings of varying height
- **Green zones** — parks, forests, agricultural land
- **Water features** — rivers, lakes, canals (water behavior affected by Coriolis)
- **Infrastructure** — transit systems, bridges, the central light column's support structures

### Coordinate System & Floating Point Strategy

A 32 km × 25 km (circumference) world will produce severe floating point precision loss if using a single global origin. This is a **critical architectural concern**.

**Strategy: Origin Rebasing (Camera-Relative Rendering)**

- The world origin (0,0,0) is periodically shifted to the player's current position.
- All positions are stored as **double-precision world coordinates** in the ECS.
- Before rendering each frame, positions are converted to **single-precision camera-relative coordinates** (subtract camera world pos, cast to float).
- This is the same technique used by Kerbal Space Program and other large-world games.
- Physics calculations for nearby objects use single-precision relative coordinates. Distant objects use reduced-fidelity simulation.

**Coordinate Convention:**

- **Z-axis**: Along the cylinder's rotational axis (0 to 32,000 m)
- **Angular position**: Stored as radians around the cylinder circumference
- **Radial position**: Distance from the cylinder axis (0 = axis, 4000 = ground level)
- Internal representation uses cylindrical coordinates (r, θ, z) for world storage, converted to Cartesian for rendering and local physics.

---

## Physics

### Rotating Reference Frame

The cylinder rotates to produce artificial gravity. Objects inside experience:

1. **Centrifugal acceleration** — acts as "gravity," pointing radially outward. At the inner wall (r = 4000m), this equals ~9.8 m/s².
2. **Coriolis acceleration** — `a_cor = -2ω × v` — deflects moving objects. This is proportional to the object's velocity in the rotating frame and is perpendicular to both the rotation axis and the velocity vector.
3. **Centrifugal gradient** — "gravity" decreases linearly with altitude. At the axis, it is zero. An object at 2000m altitude experiences ~0.5g.

The Coriolis effect is **a core gameplay mechanic**, not just a cosmetic detail. It must be perceptible and consistent for thrown objects, projectiles, falling objects, and water flow.

### Physics LOD

Not all objects need full rotating-frame physics every frame:

| Distance from Player | Physics Fidelity |
|---|---|
| 0–200 m | Full Coriolis + centrifugal, per-frame integration |
| 200–2000 m | Simplified physics, reduced tick rate (e.g., every 4th frame) |
| 2000 m+ | Static or scripted motion only; no dynamic physics |
| Unloaded cylinder | Persistent state stored, no simulation |

Physics integration should use **semi-implicit Euler** or **Verlet** for stability. The rotating-frame pseudoforces are applied as accelerations during integration — not by literally rotating the world.

### Angular Velocity Reference

For 1g at r = 4000m:

```
ω = sqrt(g / r) = sqrt(9.81 / 4000) ≈ 0.0495 rad/s
Period ≈ 126.9 seconds (~2.1 minutes per revolution)
```

---

## Rendering

### Shader-Driven Visual Strategy

The visual strategy prioritizes **shaders over high-resolution textures** to keep memory footprint low and maximize hardware compatibility.

Key shader techniques to implement:

- **Procedural texturing** — noise-based materials for terrain, concrete, vegetation
- **Atmospheric scattering** — simulated atmosphere with haze and distance fog matching the curved interior
- **Central column lighting** — the light column is a massive linear area light; approximate with a multi-sample linear light model
- **Curved horizon rendering** — the ground curves up on all sides; this must feel natural and not produce visual artifacts
- **LOD-aware shading** — distant objects use simpler shaders automatically

### Level of Detail (LOD) Policy

| Distance | Geometry | Shading | Shadows |
|---|---|---|---|
| 0–100 m | Full mesh | Full PBR + detail maps | Full shadow casting/receiving |
| 100–500 m | Reduced mesh (50% polys) | Simplified PBR | Shadow receiving only |
| 500–2000 m | Low-poly hull | Flat shading + impostor blending | No per-object shadows |
| 2000–8000 m | Billboard/impostor | Baked color | None |
| 8000 m+ | Color points or omitted | Fog blend | None |

Given the cylinder's curvature, objects on the "ceiling" (directly overhead, ~8 km away) should be rendered as a distant atmospheric texture layer, not as discrete geometry.

### The Curved World Problem

Standard rendering assumes a flat ground plane. In an O'Neill cylinder:

- The ground curves **upward** in every direction.
- The visible horizon is **above** the player, not below.
- Objects directly "overhead" are the far side of the cylinder — potentially 8 km away with an entire city visible.

**Solution: Atmospheric Fog + Cylindrical Skybox**

The interior atmosphere provides a physically motivated rendering cutoff. Light passing through kilometers of air scatters (Rayleigh scattering), producing a blue haze that increases with distance. The air density inside the cylinder is effectively uniform, so fog is a simple **omnidirectional distance-based fade** from the camera — no directional or altitude-dependent adjustments needed.

- **0–4 km**: No fog. Full geometry rendering at appropriate LOD tiers.
- **4–7 km**: Linear fog ramp. Geometry fades from full visibility to fully obscured. Billboard/impostor LOD tiers live in this range, so visual cheapness is masked by haze.
- **7 km+**: Fully fogged. No discrete geometry rendered.

Beyond the fog wall, a **cylindrical skybox texture** provides the far-side visual. This is not a static baked texture — it is regenerated infrequently (every 30–60 seconds, or on significant lighting changes such as the day/night cycle) by rendering the habitat surface from an orthographic top-down camera into a low-resolution cylindrical projection. This ensures the overhead view reflects time-of-day, city lights at night, and other large-scale changes without per-frame cost.

The fog ramp must blend seamlessly into the skybox. The skybox color at any given angle should approximate what the fogged-out geometry would look like at that distance and lighting condition.

---

## Terrain System

### Tile-Based Ground Geometry

The cylinder interior surface (~32 km × ~25 km) is divided into a grid of **500m × 500m terrain tiles**. Each tile is a distinct mesh authored in Blender as a flat surface with local terrain detail (elevation changes, drainage, paths, building foundations, lakebeds, etc.). At runtime, the vertex shader projects each tile's vertices onto the cylinder surface at r = 4000m.

**Grid dimensions**: 64 tiles (Z-axis) × 51 tiles (circumference) = **3,264 tiles** per cylinder.

Each tile's internal mesh should be subdivided to roughly **20m resolution** (~625 quads per tile), providing smooth curvature after cylindrical projection. At 4km radius, a 20m chord deviates ~1.25cm from the true arc — imperceptible at ground level.

### Tile Authoring

Tiles are authored flat in Blender and exported as glTF (or similar). A tile represents a terrain *type* — parkland, dense urban foundation, riverbank, lakeshore, residential grid, industrial, agricultural, forest floor, etc. The same tile design can appear many times in the world with varied orientation and entity population.

Tiles should be authored with edges that mate cleanly. A simple convention: all tile edges sit at a uniform base elevation, with terrain variation contained within the interior. Edge-blending between dissimilar tiles (e.g., park adjacent to urban) is handled by a set of **transition tiles** designed for specific adjacency pairs.

### GPU Curvature Strategy

Three techniques work together to produce smooth curvature from flat-authored tiles:

1. **Vertex shader cylindrical projection**: Every ground vertex is placed onto the cylinder surface mathematically. The shader receives flat (θ, z) coordinates and computes Cartesian position at the correct radius. This is the primary mechanism — it turns flat tiles into curved geometry.

2. **Correct radial normals**: Each vertex normal points radially outward from the cylinder axis. Lighting interpolation across the polygon surface produces the visual appearance of smooth curvature even where the geometric approximation is coarse.

3. **Distance-based tessellation**: For tiles near the player (< 200m), a tessellation shader subdivides the mesh further, ensuring the horizon line and nearby ground feel perfectly smooth. Distant tiles skip tessellation entirely since fog and LOD mask any faceting.

### Tile Streaming

Only tiles within the fog radius (~7 km from the player) are loaded into GPU memory. As the player moves, tiles stream in and out. A **loading ring** slightly beyond the fog distance (e.g., 8 km) preloads tiles before they become visible, preventing pop-in.

### World Layout File

The world layout is defined in a **human-readable, mod-friendly format**. It consists of two files per cylinder:

**`cylinder_0_tiles.toml`** — Tile index and metadata:

```toml
[meta]
cylinder_id = 0
grid_z = 64        # tiles along Z-axis
grid_theta = 51    # tiles around circumference
tile_size = 500.0  # meters

[tiles]

[tiles.park_01]
mesh = "tiles/park_01.glb"
description = "Open parkland with gentle hills and footpaths"

[tiles.urban_core_01]
mesh = "tiles/urban_core_01.glb"
description = "Dense city center foundation with road grid"

[tiles.river_straight]
mesh = "tiles/river_straight.glb"
description = "River channel running along Z-axis"

[tiles.transition_park_urban]
mesh = "tiles/transition_park_urban.glb"
description = "Gradual transition from park to urban"

# ... etc
```

**`cylinder_0_layout.csv`** — The 2D grid, readable in any text editor or spreadsheet:

```csv
# Row = theta index (0-50), Column = z index (0-63)
# Each cell is a tile ID from the tile index, optionally with rotation suffix: /0 /90 /180 /270
park_01,park_01,transition_park_urban,urban_core_01,urban_core_01,river_straight/90,...
park_01,park_01,transition_park_urban,urban_core_01,urban_core_01,river_straight/90,...
forest_01,forest_01,park_01,transition_park_urban,urban_core_01,river_straight/90,...
...
```

This format is intentionally simple. A modder with a text editor can rearrange the entire world, add custom tile meshes to the tile index, and create a completely different habitat layout. The CSV is directly viewable in any spreadsheet application for a visual overview of the world map.

### Entity Population per Tile

Tiles define only the base ground geometry. Buildings, vegetation, NPCs, and other entities are placed via a separate **entity placement file** per tile type, defining spawn points relative to the tile's local coordinate system. This keeps terrain shape and world content cleanly separated — a modder can re-skin the world layout without touching entity definitions, or vice versa.

---

## World Generation

### Overview

The cylinder interior layout is procedurally generated on a **256 × 204 cell grid** (Z-axis × circumference). Each cell represents approximately **125m × 123m** of interior surface. This grid is the **biome map** — it determines the high-level land use of every point on the cylinder surface.

The 500m authored terrain tiles each cover a **4×4 block** of generation cells. The generator's biome map output directly determines which tile variant is placed at each 4×4 position in the world layout file.

### Target Biome Ratios

| Biome | Target Coverage | Notes |
|---|---|---|
| Water (lakes, rivers, channels) | ~30% | Multiple lakes, connected by meandering rivers |
| Park / green space | ~20% | Mostly along water edges, plus one large contiguous forest |
| Urban / residential | ~25% | Commerce, housing, everyday life |
| Industrial / science | ~25% | Factories, labs, manufacturing |

### Generation Pipeline

Generation runs as an offline tool at development time (and is shipped separately as a modding add-on). It produces a biome map that is then translated into tile layout files. The pipeline runs in five ordered passes, where each pass constrains the next.

**Pass 1 — Water Placement (Poisson Disk + Blob Growth)**

1. Use Poisson disk sampling to place 8–12 lake seed points on the 256×204 grid with a minimum separation of ~40 cells (~5 km). This ensures lakes are evenly but organically distributed.
2. Grow each seed into a lake using randomized flood fill with a size budget. Vary sizes: 2–3 large lakes (600+ cells), 4–5 medium lakes (150–400 cells), and several small ponds (20–80 cells). Use Simplex noise to modulate the growth probability at the frontier so shorelines are irregular, not circular.
3. Connect nearby lakes with rivers using A* pathfinding with a noise-perturbed cost function. Rivers are 1 cell wide (~125m). The cost function should penalize straight runs and favor gentle curves, producing natural meanders. Rivers flow along the Z-axis or at shallow angles — avoid rivers running purely around the circumference, as Coriolis deflection would make purely circumferential flow unnatural.
4. Stop when water coverage reaches ~30%.

**Pass 2 — Green Space (Edge Growth + Forest Reservation)**

1. Reserve one contiguous rectangular region (~32×24 cells, roughly 4km × 3km) for the primary forest, placed away from the largest lake cluster.
2. Mark all non-water cells adjacent to water as park candidates with high probability (~80%).
3. Expand parks outward from water edges with decaying probability (each additional ring: 60%, 40%, 20%).
4. Fill remaining green quota with small scattered parks (3–8 cell clusters) using secondary Poisson disk placement.
5. Stop when park coverage reaches ~20%.

**Pass 3 — Urban/Industrial Fill (Simplex Noise Partitioning)**

1. Generate a single octave of low-frequency Simplex noise across the grid (wavelength ~60–80 cells, roughly 8–10 km).
2. All remaining unassigned cells become urban or industrial based on the noise value: above threshold → industrial, below → urban.
3. Tune the threshold to produce approximately equal coverage of each.
4. Industrial zones naturally cluster into contiguous districts due to the noise frequency.

**Pass 4 — Adjacency Cleanup**

1. Industrial cells directly adjacent to water → convert to urban (design choice: no heavy industry on the waterfront).
2. Eliminate isolated single-cell biome islands — absorb into the dominant neighbor biome.
3. Ensure every residential zone has connectivity to at least one water feature within ~20 cells (livability heuristic).
4. Smooth jagged biome boundaries by majority-vote filtering (a cell surrounded by 5+ neighbors of a different biome flips to match).

**Pass 5 — Tile Assignment**

1. Divide the 256×204 biome map into 64×51 blocks of 4×4 cells each.
2. For each 4×4 block, determine the dominant biome and any biome transitions present.
3. Assign a tile ID from the tile index:
   - Pure blocks (all 16 cells same biome) → standard tile for that biome.
   - Mixed blocks → select an appropriate transition tile based on which biomes are present and their spatial arrangement within the 4×4.
   - River blocks → select river tile variant matching the flow direction and curvature (straight, gentle bend, sharp bend, confluence).
4. Assign rotation (0°/90°/180°/270°) to align directional tiles (rivers, shorelines, transitions) correctly.
5. Output the final `cylinder_N_layout.csv` and `cylinder_N_tiles.toml`.

### Generator Configuration

The generator reads a **seed file** in TOML format:

```toml
[generator]
seed = 847291
grid_z = 256
grid_theta = 204

[water]
target_ratio = 0.30
lake_count_min = 8
lake_count_max = 12
lake_min_separation = 40   # cells
river_meander_factor = 0.6 # 0 = straight, 1 = very winding

[green]
target_ratio = 0.20
forest_size = [32, 24]     # cells [z, theta]
waterfront_park_probability = 0.80
park_decay_rate = 0.5       # per ring

[urban]
target_ratio = 0.25
noise_wavelength = 70       # cells

[industrial]
target_ratio = 0.25
waterfront_allowed = false
```

Modders can tweak these parameters and regenerate entirely different cylinder layouts. The seed value ensures reproducibility.

### Modding Distribution

The procedural generator ships as a separate standalone tool (not embedded in the game runtime). It:

1. Reads a seed configuration TOML file.
2. Outputs a biome map visualization (PNG) for preview.
3. Outputs the `cylinder_N_layout.csv` and `cylinder_N_tiles.toml` files ready for the game to load.
4. Optionally outputs a detailed biome map CSV at the full 256×204 resolution for modders who want to hand-edit at fine resolution before tile assignment.

The canonical game ships with two pre-generated, hand-tuned cylinders. The generator is the starting point for those layouts, with manual edits applied afterward for narrative purposes.

---

## Entity Component System Design

### Core Principles

- **Components are plain data.** No logic in components.
- **Systems operate on component sets.** All logic lives in systems.
- **Prefer many small components** over few monolithic ones.
- **Tag components** (empty structs) for categorical queries.

### Component Categories

#### Transform & Spatial

```
CylindricalPosition   { double r, theta, z; }
LocalTransform         { mat4 transform; }          // camera-relative, computed each frame
Velocity               { vec3 v_local; }            // in rotating frame
AngularVelocity        { vec3 omega; }
BoundingVolume         { enum type; vec3 extents; }
```

#### Rendering

```
MeshInstance           { MeshHandle mesh; uint8_t lod_level; }
Material               { MaterialHandle material; }
ShaderOverride         { ShaderHandle shader; }      // per-entity shader override
Impostor               { TextureHandle billboard; }  // for distant LOD
Visible                { }                           // tag: currently in frustum
```

#### Physics

```
RigidBody              { float mass; float restitution; }
PhysicsLOD             { enum tier; }                // current physics fidelity tier
StaticBody             { }                           // tag: immovable
GravityAffected        { }                           // tag: subject to centrifugal/Coriolis
```

#### World

```
Building               { uint16_t floors; float height; }
Vegetation             { enum species; float growth; }
WaterBody              { float volume; float flow_rate; }
LightSource            { vec3 color; float intensity; }
CentralColumn          { float z_start; float z_end; } // light column segment
```

#### Game State

```
Interactable           { enum type; }
InventoryItem          { uint32_t item_id; }
NPC                    { uint32_t dialogue_tree; }
PersistentState        { }                           // tag: survives cylinder unload
```

#### Cylinder Management

```
CylinderID             { uint8_t id; }               // 0 or 1
CrossCylinderRef       { uint32_t entity_id; uint8_t source_cylinder; }
```

### Key Systems

| System | Components Queried | Tick Rate | Description |
|---|---|---|---|
| PhysicsSystem | CylindricalPosition, Velocity, RigidBody, GravityAffected | Per-frame | Full rotating-frame integration with Coriolis |
| PhysicsLODSystem | CylindricalPosition, PhysicsLOD | Every 30 frames | Reassigns physics fidelity tiers based on distance |
| RenderCullSystem | CylindricalPosition, BoundingVolume | Per-frame | Frustum culling + LOD assignment |
| MeshLODSystem | CylindricalPosition, MeshInstance | Every 10 frames | Swaps mesh LOD based on distance |
| OriginRebaseSystem | — | When player moves >1000m from origin | Shifts all positions to re-center origin |
| CylinderTransitionSystem | — | On trigger | Serializes persistent state, unloads current cylinder, loads next |
| InputSystem | — | Per-frame | Polls SDL3, maps to abstract actions |
| PlayerMovementSystem | PlayerTag, CylindricalPosition, Velocity | Per-frame | Walk/run/jump with rotating-frame correction |
| WaterFlowSystem | WaterBody, CylindricalPosition | Every 4 frames | Simulates Coriolis-affected water flow direction |
| LightColumnSystem | CentralColumn | Per-frame | Day/night cycle, light color/intensity |

---

## Input Abstraction

SDL3 handles raw input. The game uses an **action mapping layer**:

```
// Abstract actions, not raw keys
enum class Action {
    MoveForward, MoveBackward, StrafeLeft, StrafeRight,
    Jump, Crouch, Sprint,
    Interact, UseItem,
    LookUp, LookDown, LookLeft, LookRight,
    Pause, ToggleMap,
    // ...
};
```

Input bindings are stored in a configuration file and are remappable at runtime. Support keyboard+mouse and gamepad from the start.

---

## Cylinder Transition & Persistent State

When the player moves between cylinders:

1. **Serialize** all entities tagged `PersistentState` in the current cylinder to a binary snapshot.
2. **Unload** the current cylinder's world data, meshes, and non-persistent entities.
3. **Load** the target cylinder from disk/asset files.
4. **Deserialize** persistent state for the target cylinder from its last snapshot.
5. **Spawn** the player at the designated entry point.

The snapshot format should be compact and versioned. Consider a flat binary format over JSON for speed.

---

## Build & Project Structure

```
eschaton/
├── CMakeLists.txt
├── CLAUDE.md                    # This file
├── src/
│   ├── main.cpp
│   ├── core/                    # Engine core: app lifecycle, time, config
│   ├── ecs/                     # Component definitions, system base classes
│   │   ├── components/
│   │   └── systems/
│   ├── physics/                 # Rotating-frame physics, collision
│   ├── render/                  # bgfx renderer, mesh management, LOD
│   ├── world/                   # Cylinder loading, origin rebasing, transitions
│   ├── input/                   # SDL3 input polling, action mapping
│   ├── audio/                   # (TBD)
│   └── game/                    # Game-specific logic, NPCs, items
├── shaders/
│   ├── varying.def.sc           # bgfx shared varying definitions
│   ├── vs_*.sc                  # Vertex shaders
│   └── fs_*.sc                  # Fragment shaders
├── assets/
│   ├── meshes/
│   ├── textures/
│   └── data/                    # World layout, entity definitions, config
├── third_party/
│   ├── bgfx/
│   ├── sdl3/
│   └── entt/
└── tools/                       # Asset pipeline scripts, shader compiler wrappers
```

---

## Coding Conventions

- **Naming**: `snake_case` for functions and variables, `PascalCase` for types and components, `UPPER_CASE` for constants.
- **Headers**: Use `#pragma once`. Prefer forward declarations over includes.
- **Memory**: Prefer stack allocation and contiguous containers (`std::vector`). Minimize `new`/`delete`. Use smart pointers for ownership semantics when heap allocation is necessary.
- **Error handling**: No exceptions. Use return codes or `std::expected` (C++23) / `std::optional`.
- **Comments**: Explain *why*, not *what*. Document non-obvious physics formulas.
- **Formatting**: clang-format with a project `.clang-format` file.

---

## Performance Targets

| Metric | Target |
|---|---|
| Frame rate | 60 fps sustained on GTX 1060 / RX 580 class hardware |
| Minimum GPU | GTX 560 / HD 6870 class (reduced settings) |
| RAM usage | < 4 GB for one loaded cylinder |
| Draw calls per frame | < 2000 (bgfx helps with batching) |
| Physics entities per frame | < 500 active (full fidelity) |
| Load time (cylinder swap) | < 5 seconds |

---

## Open Questions & Future Decisions

- [ ] Audio middleware selection (OpenAL Soft vs miniaudio vs FMOD)
- [ ] Asset pipeline: glTF import tooling, texture compression format
- [ ] Vegetation rendering strategy (billboard grass vs geometry instancing)
- [ ] Water rendering approach (screen-space reflections vs planar reflections)
- [ ] NPC/AI architecture
- [ ] Save game format
- [ ] Procedural generation vs hand-authored world layout (or hybrid)
- [ ] Network/multiplayer (or strictly single-player?)
- [ ] Anti-cheat / modding considerations

---

## Key Reference Material

- **O'Neill Cylinder Physics**: ω = sqrt(g/r), Coriolis acceleration = -2(ω × v), centrifugal gradient is linear with radius
- **bgfx documentation**: https://bkaradzic.github.io/bgfx/
- **bgfx shaderc**: https://bkaradzic.github.io/bgfx/tools.html#shader-compiler-shaderc
- **SDL3 documentation**: https://wiki.libsdl.org/SDL3
- **EnTT documentation**: https://github.com/skypjack/entt/wiki
- **Origin rebasing technique**: Used in KSP, Space Engine, Outerra — shift world origin to camera each frame
- **Rotating habitat physics**: Clarke, Arthur C. "Rendezvous with Rama" (fictional reference); O'Neill, Gerard K. "The High Frontier" (engineering reference)
