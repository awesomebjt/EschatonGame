# World Pipeline — Byte-Level Reference & Art Contract

Full detail backing the "World Data Formats" summary in the root `CLAUDE.md`. Read
that section first for the directory layout and the high-level shape; this file is
the byte-exact reference kept out of the root file so it doesn't load into every
session's context.

## Directory Layout

```
assets/world/
├── manifest.json            map constants, palette, building catalogue, chunk table
├── prototypes.bin           20 types × LOD0/LOD1, local space   (later: prototypes.json + glb)
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

## Prototype Local Space

Origin at the midpoint of the street-facing edge at ground level, +X along the
street, +Y into the block, +Z up. An instance is placed with
`translate(x,y,z) · rotateZ(rot)`. No scaling, ever — every type has fixed dimensions.
Round-trip from placed footprint to instance and back is exact (verified to
0.000000 m).

## Replacing Placeholder Art

The instance table is the durable data; the prototypes are a reference that currently
resolves to boxes. To upgrade:

1. Author models against the footprint quads in `manifest.json` (importable into
   Blender as guides).
2. Replace `prototypes.bin` with `prototypes.json` indexing glTF files per
   `(type, lod)`, keeping type IDs stable.
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
