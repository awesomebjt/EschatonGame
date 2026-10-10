# Building prototypes: integration guide

For whoever (or whatever) wires `tools/prototypes.py` into the engine. Read
`docs/WORLD_PIPELINE.md` first for the world data formats; this document covers only what
changes when the placeholder boxes become detailed buildings.

## Integration status (2026-10-09)

Wired in, against `world/` (this repo's world directory, not `assets/world/cylinder_0/`).
Where the integration departed from the guide below:

- **UV phase.** As delivered, `u` ran continuously around the perimeter, so `frac(u / 5)`
  put LOD2/3 windows half a bay off the real openings on most walls. Each wall's UVs are
  now phased so its bay centres sit at `u = 2.5 (mod 5)` (checked: 0 of 3,500 LOD1
  windows off the grid).
- **LOD2/3 walls** are three full-height quads per wall: the run of bays with real UVs,
  and the corner piers with a constant UV `(0, 0)` (masonry). Without this the facade
  material put half-windows on every corner. 18–22 triangles per building, down from 34–72.
- **Collision** is a fourth part, `prototypes_collision.bin`, built from the plan: wall
  slabs with the ground-floor doors open, the carriage passage as a tunnel, pillars for
  the colonnade, the roof. 74–556 triangles per type. Players can walk into ground floors.
- **`export_chunks.py`** no longer writes the v1 `prototypes.bin` (it would overwrite v2);
  it calls `prototypes.build_all` instead.
- **LOD selection**: per instance in a 7 × 7 chunk ring; outside it, everything is LOD3 in
  one whole-cylinder instanced draw per type (the vertex shader drops ring instances).
  There is no per-chunk middle tier because nothing between the ring and the far draw
  needs one yet.
- **Interiors** load with everything else (8 MB); not lazy or evictable until chunks stream.
- **Door glazing** is a fifth part. Doors are walkable, so their panes are drawn only for
  instances whose interior isn't (beyond `interior_max_distance`); closer, the doorway is
  open onto the real ground-floor room. The carriage passage no longer gets a pane at all:
  it is an open tunnel into the courtyard.
- **Top slab.** As delivered, the top storey's slab put an upward face exactly at roof
  height, which z-fought with the exterior roof whenever interiors were drawn (< 150 m).
  Only its ceiling is emitted now.

Not done yet: shadow proxies (there is no building shadow map at all yet), the backdrop
fragment discard (no backdrop yet), `plans` parsing (nothing consumes it yet), variants.

## What this changes, and what it does not

`prototypes.py` replaces the meshes that building instances resolve to. That is all.

**Unchanged — do not touch:**

- `paris_city.py`, the world generator
- `export_chunks.py` and every `chunk_III_JJJ.bin`
- the instance table format: still `(type, x, y, z, rot)`, 20 bytes
- the streamer, the chunk terrain meshes, the backdrop bake
- prototype local space: origin at the midpoint of the street-facing edge, +X along the
  street, +Y into the block, +Z up, placed with `translate · rotateZ`, **never scaled**

**Changed:**

- `prototypes.bin` is format version **2** and is no longer compatible with the v1 reader in
  `WORLD_PIPELINE.md` §3.3 — the per-mesh header gained a `part` field
- a second file, `prototypes_interior.bin`, holds the interior shells for on-demand loading
- buildings need their own vertex layout (48 bytes, with tangents and UVs) distinct from the
  28-byte chunk terrain layout
- LOD selection becomes per-instance near the player instead of per-chunk everywhere

---

## Running it

```bash
cd tools && python3 prototypes.py --out ../world/ --bake-tile --report
```

Plain CPython, numpy only, no Blender. Runs in a few seconds. `--report` prints a per-type
triangle breakdown; `--bake-tile` also writes `facade_tile.png`.

Output:

```
prototypes.bin            31.6 MB   exterior + glazing, all four LOD bands
prototypes_interior.bin    8.2 MB   interior shells (LOD0 only)
prototypes_collision.bin    37 KB   solid massing for physics
prototypes.json             ~1 MB   index, LOD distances, facade tile spec, building plans
facade_tile.png            357 KB   optional: one 5 x 5 m bay, RGB masonry + A depth mask
```

Rebuild whenever `BUILDING_TYPES` changes in `paris_city.py`. The script reads the catalogue
from the generator directly, so the two can never disagree about footprints or heights.

---

## Formats

### `prototypes.bin` / `prototypes_interior.bin`

```
0   4   magic "PROT"
4   4   u32 version (2)
8   4   u32 meshCount
then per mesh:
    u16 type; u16 lod; u16 part; u32 vertexCount; u32 indexCount;
    vertices, indices
```

```cpp
#pragma pack(push, 1)
struct ProtoHeader { char magic[4]; uint32_t version, meshCount; };          // 12
struct ProtoMesh   { uint16_t type, lod, part; uint32_t vtx, idx; };         // 14
struct BuildVertex { float pos[3], nrm[3], tan[4], uv[2]; };                 // 48
#pragma pack(pop)
static_assert(sizeof(ProtoMesh) == 14 && sizeof(BuildVertex) == 48, "");
```

`part`: `0 = exterior`, `1 = glazing`, `2 = interior`, `3 = collision`, `4 = door glazing`.

Note `ProtoMesh` is 14 bytes and therefore unaligned — read it field by field or memcpy into
a local, don't reinterpret_cast a pointer into the middle of the buffer on platforms that
care.

Tangent `w` is the handedness sign, always `+1` here. Tangents are emitted from the script
rather than derived at load, because every wall is planar and its tangent is exactly the
wall direction — better than any reconstruction, and free.

UVs are **metric**: `u` is distance along the wall from its start, `v` is height above the
building base. So `frac(u / 5)` and `frac(v / 5)` address the position within a window bay.
This is the keystone of the whole scheme — the geometry, the texture tile and the parallax
shader all derive from the same numbers, so a parallax window at 600 m sits exactly where
the real opening was at 300 m.

### `prototypes.json`

```jsonc
{
  "version": 2,
  "vertex_stride": 48,
  "vertex_layout": ["position:3f", "normal:3f", "tangent:4f", "texcoord0:2f"],
  "files": { "shell": "prototypes.bin", "interior": "prototypes_interior.bin" },
  "parts": { "0": "exterior", "1": "glazing", "2": "interior" },
  "lod_bands": [ { "lod": 0, "max_distance": 120, "openings": true }, ... ],
  "interior_max_distance": 150,
  "facade_tile": {
    "size": 5.0,
    "window": { "width": 1.0, "height": 2.0, "sill": 1.0, "arch_radius": 0.5 },
    "recess": 0.2,
    "brick": { "length": 0.21, "height": 0.065, "mortar": 0.012 }
  },
  "meshes": [ { "type": 7, "lod": 0, "part": 0, "file": "shell",
                "vertices": 44160, "indices": 44160, "triangles": 14720,
                "aabb": [...] } ],
  "plans": [ /* see below */ ]
}
```

### `plans` — keep this, the engine wants it

Every type's plan is exported whether or not you use these meshes: wall segments with their
lengths and endpoints, window bay centres per wall, door positions and widths, floor heights,
courtyard pillar positions, wall thickness. Uses:

- **collision** — footprint plus wall thickness plus floor heights gives box colliders and
  floor planes without touching the mesh
- **interior mapping** — per-window seeds for the parallax interior shader
- **night lighting** — which windows light up, and where emissive quads go
- **NPC pathing** — door positions are building entrances
- **gameplay** — floor heights tell you which storey an entity is on

When real art replaces these meshes, the plans stay. They describe the building, not the
mesh.

---

## Engine work, in order

**1. Second vertex layout.** Terrain stays 28 bytes (position, normal, colour). Buildings are
48 bytes (position, normal, tangent, UV). Two layouts, selected by which file the buffer came
from.

```cpp
buildingLayout.begin()
    .add(bgfx::Attrib::Position,  3, bgfx::AttribType::Float)
    .add(bgfx::Attrib::Normal,    3, bgfx::AttribType::Float)
    .add(bgfx::Attrib::Tangent,   4, bgfx::AttribType::Float)
    .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
    .end();
```

**2. Prototype registry.** Load `prototypes.json`, then `prototypes.bin`. Key meshes by
`(type, lod, part)`. Load `prototypes_interior.bin` lazily — the first time any instance comes
within `interior_max_distance`, or on a background thread at level load if the 8 MB is
affordable. Keep it evictable.

**3. Per-instance LOD near the player.** Previous guidance was one LOD per chunk, which was
right when LOD0 was 12 triangles. At 15–23k it is not: in the chunk you are standing in, one
building is 8 m away and another is 490 m. So:

- chunks within the 3 × 3 ring: select LOD per instance from squared distance
- chunks beyond: one LOD for the whole chunk, chosen from the chunk centre

Bucket by `(type, variant, lod, part)` and submit one instanced draw per bucket. A few
hundred instances to sort per frame in the near ring; negligible.

**4. Hysteresis.** Give each threshold ±10% so a building on a boundary doesn't thrash when
the player paces back and forth. Use squared distances throughout; don't sqrt per instance.

**5. Glazing is always drawn when the band has openings.** It carries the interior-mapping
shader (a parallax interior box per window). This is what stops windows being holes onto
nothing when the interior part is not loaded — which is the normal case beyond 150 m. Draw
glazing after the exterior, no backface culling needed since the pane faces out.

**6. LOD2/LOD3 facade material.** No openings in the geometry; the material reconstructs
them. Read `facade_tile` from the JSON and either build the pattern procedurally in the
fragment shader (preferred; matches the project's shaders-over-textures stance) or sample the
baked `facade_tile.png`, whose alpha channel is the parallax depth mask. Because UVs are
metric and the tile is exactly `BAY` metres, the material tiles correctly on every wall
including the tapered types, with no UV packing and no per-type texture.

**7. Interior loading rule.** Load and draw `part = 2` only for instances within
`interior_max_distance` (150 m) or when the player is inside the building. The interior is
roughly a third of the triangles and is invisible from outside. Draw it double-sided or with
reversed winding — you are inside the surface.

**8. Shadow proxy.** Walls are 12 cm thick. A shadow map rendering the real shell will leak
and acne. Render shadows from a solid box per instance (the footprint extruded to height),
not from the detail mesh. This is faster anyway.

**9. Backdrop transition.** The backdrop cylinder is at radius 3,947 m and therefore passes
over your head and wraps the ground you are standing on — you cannot "switch to it" at a
distance. It needs a fragment discard for anything closer than the transition radius, with a
blend band matching the fog ramp. Do this before widening any LOD band.

---

## Budget

Measured, default catalogue:

| Type | LOD0 | LOD1 | LOD2/3 |
|---|---|---|---|
| Cube (40 m) | 15,375 | 6,620 | 66 |
| Court80 (35 m, courtyard) | 23,126 | 11,236 | 2,366 |
| Sliver (20 m) | 4,539 | 1,948 | 34 |

LOD0 figures include the interior part. Courtyard types keep a higher LOD2/3 floor because the
courtyard void survives; the colonnade itself is dropped beyond LOD1 and replaced with plain
inner faces.

Target frame cost with the bands as shipped:

| Band | Buildings in view | Triangles |
|---|---|---|
| 0–120 m | ~18 | ~360k |
| 120–400 m | ~180 | ~450k |
| 400–1200 m | ~1,800 | ~360k |
| 1.2–4 km | ~20,000 | ~240k |

About 1.4M triangles for all buildings, which leaves room for terrain and the backdrop under
a 2M budget on the target hardware. If you widen the LOD0 band, check this table again — the
first band is the one that explodes.

---

## Gotchas

- **Occlusion culling gets much weaker.** Hollow buildings with pillared courtyards mean you
  can see through a building into its yard and out the far side. Facades are no longer
  occluders. Distance LOD has to carry the budget alone; this is the main argument for the
  narrow LOD0 band.
- **Thin walls and depth precision.** 12 cm separations at kilometre draw distances will
  z-fight on some hardware. If you see it, raise `WALL_T` to 0.20–0.25 in the script and
  rebuild. Nothing downstream cares.
- **Never scale instances.** Every type has fixed dimensions, which is the whole reason the
  catalogue instances cleanly. A scaled instance breaks the metric UVs and the brick size.
- **The height map ceiling is 112 m**, set by the large monument domes. Buildings reach 40 m,
  so the backdrop bake is unaffected by this change — no rebake needed.
- **Draw call count rises.** Up to 20 types × up to 3 parts per chunk in the near ring. Sort
  and bucket properly, or the near ring alone will blow the call budget before the triangle
  budget.

---

## Validation gates

`prototypes.py` runs four checks and prints `validation clean` or the failures. They catch
nearly every popping and clipping bug before it reaches the engine:

1. all LOD bands of a type agree on outer bounds (±0.5 m, allowing for cornice and reveals)
2. the interior AABB sits inside the exterior AABB
3. every window bay respects the minimum corner pier (0.6 m) on its wall
4. no window collides with a door opening

If you change bay pitch, window size, door width or the corner pier rule, run with `--report`
and confirm the checks still pass before exporting.

---

## Checklist

- [x] `prototypes.json` parsed; registry keyed by `(type, lod, part)`
- [x] 48-byte building layout alongside the 28-byte terrain layout
- [x] v2 reader handles the 14-byte unaligned mesh header
- [ ] interior file loaded lazily, evictable — loaded up front for now
- [x] per-instance LOD in the near ring (7 × 7), LOD3 beyond, 10% hysteresis
- [x] instances bucketed by `(type, lod, part)`; one instanced draw each (no variants yet)
- [x] glazing drawn whenever the band has openings; interior-mapping shader bound
- [x] LOD2/3 facade material reads `facade_tile` and lands windows on the bay grid
- [ ] shadows rendered from box proxies, not the detail shell — no building shadows yet
- [ ] backdrop fragment discard inside the transition radius — no backdrop yet
- [x] collision from a solid massing part, not the detail meshes
- [x] `docs/WORLD_PIPELINE.md` and the LOD table in `CLAUDE.md` updated to version 2

---

## When real art arrives

This script becomes the greybox generator rather than the shipping asset path. Replace the
`files` block in `prototypes.json` with per-`(type, lod, part)` glTF paths, load with cgltf
into the same buffers, and keep everything else. The art contract is in `WORLD_PIPELINE.md`
§6.1; the `plans` block in this file is the precise version of it — footprint quads, bay
centres, door positions and floor heights that any replacement model must honour.
