# World Pipeline — Byte-Level Reference & Art Contract

Full detail backing the "World Data Formats" summary in the root `CLAUDE.md`. Read
that section first for the directory layout and the high-level shape; this file is
the byte-exact reference kept out of the root file so it doesn't load into every
session's context.

## Directory Layout

```
assets/world/
├── manifest.json            map constants, palette, building catalogue, chunk table
├── prototypes.json          prototype index: LOD bands, facade tile, per-type plans
├── prototypes.bin           exterior + glazing, 20 types × LOD0–3        (format v2)
├── prototypes_interior.bin  interior rooms, LOD0 only                    (format v2)
├── prototypes_collision.bin solid massing the player collides with       (format v2)
└── chunk_III_JJJ.bin        terrain mesh + building/monument instances, chunk-local
```

`manifest.json` is the human-readable index: map constants (map size, chunk size, wrap
width), the material colour palette, the building catalogue with exact footprints and
heights, roundabout specs (S/M/L radii and counts), and the chunk table. C++ should
read map constants from here rather than hardcoding them — see the "World data" rule
under Coding Conventions in the root `CLAUDE.md`.

## Binary Chunk Format

```cpp
#pragma pack(push, 1)
struct ChunkHeader {                      // 40 bytes, magic "CHNK", version 1
    char magic[4]; uint32_t version;
    int32_t i, j; float centreX, centreY;
    uint32_t vertexCount, indexCount, buildingCount, monumentCount;
};
struct Vertex       { float pos[3]; float nrm[3]; uint8_t rgba[4]; };   // 28
struct BuildingInst { uint16_t type; uint16_t lodHint; float x, y, z, rot; }; // 20
struct MonumentInst { uint16_t size; uint16_t pad;     float x, y, z; };      // 16
#pragma pack(pop)
static_assert(sizeof(ChunkHeader) == 40 && sizeof(Vertex) == 28, "");
```

The vertex block maps 1:1 onto a bgfx layout and needs no conversion:

```cpp
layout.begin()
      .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
      .add(bgfx::Attrib::Normal,   3, bgfx::AttribType::Float)
      .add(bgfx::Attrib::Color0,   4, bgfx::AttribType::Uint8, true)
      .end();                                   // stride 28
```

Use `BGFX_BUFFER_INDEX32` — dense chunks exceed 65,535 vertices.

All positions in a chunk file are relative to the chunk centre. Measured sizes: ~35 KB
per chunk, ~110 MB for the full map, of which the building instances for all ~375k
buildings are about 7 MB.

## Prototype Format (v2)

Written by `tools/prototypes.py` (also run at the end of every `export_chunks.py`
export). Design notes, budgets and validation gates: `tools/PROTOTYPES_INTEGRATION.md`.

```cpp
#pragma pack(push, 1)
struct ProtoHeader { char magic[4]; uint32_t version, meshCount; };   // "PROT", 2
struct ProtoMesh   { uint16_t type, lod, part; uint32_t vtx, idx; };  // 14, unaligned
struct BuildVertex { float pos[3], nrm[3], tan[4], uv[2]; };          // 48
#pragma pack(pop)
```

Each mesh is a `ProtoMesh` followed by its vertices and `u32` indices. `part` is
`0` exterior, `1` window glazing, `2` interior, `3` collision, `4` door glazing; the
three files hold parts {0, 1, 4}, {2} and {3}. Door panes are separate so the engine
can drop them where the interior is drawn and the player can walk in; the courtyard
carriage passage is an open tunnel and has no pane. Everything after the 14-byte header is unaligned — copy, don't
cast. Buildings get their own bgfx layout (Position 3f, Normal 3f, Tangent 4f,
TexCoord0 2f); chunk terrain keeps the 28-byte one above.

UVs are metric: `u` runs along the wall, `v` up from the base. Each wall's `u` is
phased so its window bay centres fall at `u = bay/2 (mod bay)` with `bay` = 5 m, so
`frac(uv / bay)` addresses the bay on every wall. LOD2/3 walls have no openings; the
run of bays carries real UVs and the corner piers a constant `(0, 0)` (plain
masonry), and the facade material redraws the windows from `facade_tile` in
`prototypes.json`. Tangent is the wall direction, `w = +1`.

The collision part is built from the building plan: the outer walls as closed
`WALL_T` slabs with gaps at the ground-floor doors (full door height, so only the
arch spandrels are lost), the carriage passage as a walled and roofed tunnel to the
courtyard, the colonnade as square pillars, and the roof. Windows are solid.

## Prototype Local Space

Origin at the midpoint of the street-facing edge at ground level, +X along the
street, +Y into the block, +Z up. An instance is placed with
`translate(x,y,z) · rotateZ(rot)`. No scaling, ever — every type has fixed dimensions.
Round-trip from placed footprint to instance and back is exact (verified to
0.000000 m).

## Replacing Placeholder Art

The instance table is the durable data; the prototypes are a reference that currently
resolves to the greybox detail models from `prototypes.py`. To upgrade:

1. Author models against the footprint quads in `manifest.json` (importable into
   Blender as guides).
2. Replace the `files` block in `prototypes.json` with glTF paths per
   `(type, lod, part)`, keeping type IDs stable. The `plans` block (walls, bay
   centres, doors, floor heights) is the precise art contract; keep the bay grid
   and UV phasing so the coarse-LOD facade material still lines up.
3. Load with cgltf into the same buffers. Streamer, instancing and chunk files are
   untouched.
4. Add **variants**: pick among several models per type from a hash of the instance
   position — deterministic, stable across sessions, zero bytes on disk. Rows of
   identical buildings will give the placeholder away faster than low polycount will.
   Instances must be sorted by `(type, variant)`, which is the same load-time pass
   that already sorts by type.

**Art contract, non-negotiable parts:** stay inside the footprint quad, match the
catalogue height, model at true metres, keep the 2 × 3 m passage and ~10 m wings on
courtyard types. Ornament may overhang by about a metre — there is 3 m of curb and 4 m
of alley — but buildings are placed with party walls touching, so anything more
intersects a neighbour.
