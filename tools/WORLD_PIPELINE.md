# World pipeline: generator → binary chunks → SDL3 / bgfx

How the procedural city gets from the Python generator into the engine, and how the
placeholder boxes get replaced with real art later without changing anything downstream.

---

## 1. The shape of the pipeline

```
paris_city.py ──► export_chunks.py ──► world/                 ──► engine
  (generator)       (no Blender)         manifest.json             loader
                                         prototypes.bin            chunk streamer
                                         chunk_III_JJJ.bin         bgfx renderer

paris_city.py ──► render_map.py   ──► backdrop colour plate  ──► POM cylinder shader
              └─► bake_height.py  ──► backdrop height map
```

Blender is **not** in the runtime path. `export_chunks.py` imports the generator directly
and runs in plain CPython (numpy only). Blender is still useful for looking at the world
and for baking the backdrop textures, but nothing the engine loads passes through a .blend.

Two properties make this work:

**Determinism.** Everything derives from `SEED`. Hubs, boulevards, river and roundabout
sizes are computed for the entire map on every run and are identical every time; only the
requested `REGION` is filled in with streets and buildings. Regions exported separately
agree with each other. Verified: exporting the window at x ∈ [0, 1004] and the window at
x ∈ [25100, 26104] produces byte-identical building sets after translation.

**Instanceability.** There are exactly 20 building types with fixed dimensions. A placed
building is fully described by *(type, position, Z rotation)* — no scaling, no per-instance
geometry. So the engine ships 20 meshes and a table of ~375k transforms rather than 375k
meshes. The instance table for the whole map is about 7 MB.

---

## 2. World coordinates

| | |
|---|---|
| Units | metres |
| X | around the cylinder, **wraps** at 25,100 m (50 chunk columns × 502 m) |
| Y | along the axis, 0 … 32,000 m, does **not** wrap (1 km green border at each end) |
| Z | up, away from the inner surface (toward the axis if gravity is radial) |
| Cylinder radius | 25100 / 2π ≈ 3,994.8 m (diameter ≈ 7,990 m — Island Three proportions) |
| Chunk | 502 × 500 m; 50 columns × 64 rows = 3,200 chunks |

The generator is Z-up. bgfx imposes no world convention — you build your own view and
projection matrices — so the simplest thing is to keep Z-up throughout and be consistent in
`bx::mtxLookAt` / `bx::mtxProj`. If you'd rather work Y-up, convert once at load
(`{x, z, -y}` for positions and normals) and never again.

**Every X difference in gameplay code must wrap.** The shortest arc, not the raw
subtraction:

```cpp
constexpr float kMapW = 25100.0f;
inline float wrapDX(float dx) { return dx - kMapW * std::round(dx / kMapW); }
inline float wrapX (float x ) { float m = std::fmod(x, kMapW); return m < 0 ? m + kMapW : m; }
```

Chunk column indices wrap the same way: `col = ((i % 50) + 50) % 50`.

---

## 3. File formats

All little-endian, tightly packed, no padding beyond what's written below.

### 3.1 `manifest.json`

```jsonc
{
  "version": 1,
  "map": { "width": 25100, "height": 32000, "wrap_x": true,
           "chunk_x": 502, "chunk_y": 500, "columns": 50, "rows": 64,
           "green_border": 1000, "cylinder_radius": 3994.79 },
  "materials": { "0": [0.60,0.52,0.39], "1": [...], ... },   // id → linear-ish RGB
  "building_types": [
    { "name": "Wedge", "front": 40, "back": 5, "depth": 40, "height": 30,
      "courtyard": false,
      "footprint": [[-20,0],[20,0],[2.5,40],[-2.5,40]] }     // local space, CCW
  ],
  "courtyard": { "wall": 10, "passage_w": 2, "passage_h": 3, "threshold": 60 },
  "monuments": { "platform_height": 1.5,
                 "S": {"radius":1.5,"height":5}, "M": {"radius":3,"height":10},
                 "L": {"radius":65,"drum_height":40,"dome_radius":65} },
  "roundabouts": [ {"name":"S","road_radius":45,"platform_radius":15,"share":31}, ... ],
  "vertex_stride": 28,
  "chunks": [ { "i":42, "j":28, "file":"chunk_042_028.bin", "centre":[21334,14250],
                "vertices":616, "triangles":328, "buildings":140, "monuments":0 } ]
}
```

The `chunks` array is appended to as you export more regions, so it is the authoritative
list of what exists on disk.

### 3.2 `chunk_III_JJJ.bin`

```
offset  size  field
0       4     magic "CHNK"
4       4     u32   version (1)
8       8     i32   chunk_i, chunk_j
16      8     f32   centre_x, centre_y        (map coordinates of the chunk centre)
24      16    u32   vertexCount, indexCount, buildingCount, monumentCount
40      ...   vertices    (28 bytes each)
        ...   indices     (u32 each)
        ...   buildings   (20 bytes each)
        ...   monuments   (16 bytes each)
```

```
vertex   { f32 pos[3]; f32 nrm[3]; u8 rgba[4]; }              // 28 bytes
building { u16 type; u16 lodHint; f32 x, y, z; f32 rot; }     // 20 bytes
monument { u16 size; u16 pad;     f32 x, y, z; }              // 16 bytes
```

Positions in a chunk file — mesh vertices *and* instances — are **relative to the chunk
centre**. The world transform of a chunk is therefore a pure translation, which is exactly
what the snap-to-origin scheme needs: when you rebase, you change 3,200 translations and
touch no vertex data.

The terrain mesh in a chunk covers ground, parks, water, islands, roads, bridge decks,
roundabout roadways and platforms. It is flat-shaded: vertices are duplicated per face so
each triangle carries a hard normal, and the material colour is baked into `rgba`. One
draw call per chunk, one shader, no textures, until you replace it.

`lodHint` is reserved (always 0). `monument.size` is 0 = S, 1 = M, 2 = L.

### 3.3 `prototypes.bin`

```
0   4   magic "PROT"
4   4   u32 version (1)
8   4   u32 meshCount        // 2 × 20 = 40: every type at LOD0 and LOD1
then per mesh:
    u16 type; u16 lod; u32 vertexCount; u32 indexCount;
    vertices (28 bytes each), indices (u32 each)
```

Prototype local space:

- origin at the **midpoint of the street-facing edge**, on the ground plane
- +X runs along the street (front edge spans −front/2 … +front/2)
- +Y points away from the street, into the block
- +Z up; the building occupies 0 … `height`

An instance is placed with `translate(x, y, z) · rotateZ(rot)`. Nothing else. I verified
the round trip against the generator's own footprints: maximum vertex error 0.000000 m.

---

## 4. Loading it in C++

### 4.1 Structs

Match the file layout exactly and assert it:

```cpp
#pragma pack(push, 1)
struct ChunkHeader {
    char     magic[4];          // "CHNK"
    uint32_t version;
    int32_t  i, j;
    float    centreX, centreY;
    uint32_t vertexCount, indexCount, buildingCount, monumentCount;
};
struct Vertex        { float pos[3]; float nrm[3]; uint8_t rgba[4]; };
struct BuildingInst  { uint16_t type; uint16_t lodHint; float x, y, z, rot; };
struct MonumentInst  { uint16_t size; uint16_t pad;     float x, y, z; };
#pragma pack(pop)

static_assert(sizeof(ChunkHeader)  == 40, "");
static_assert(sizeof(Vertex)       == 28, "");
static_assert(sizeof(BuildingInst) == 20, "");
static_assert(sizeof(MonumentInst) == 16, "");
```

### 4.2 Vertex layout

```cpp
bgfx::VertexLayout layout;
layout.begin()
      .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
      .add(bgfx::Attrib::Normal,   3, bgfx::AttribType::Float)
      .add(bgfx::Attrib::Color0,   4, bgfx::AttribType::Uint8, true)   // normalized
      .end();
assert(layout.getStride() == 28);
```

The file's vertex block can go straight to the GPU with no conversion:

```cpp
const bgfx::Memory* vmem = bgfx::copy(vertexData, header.vertexCount * sizeof(Vertex));
chunk.vbh = bgfx::createVertexBuffer(vmem, layout);

const bgfx::Memory* imem = bgfx::copy(indexData, header.indexCount * sizeof(uint32_t));
chunk.ibh = bgfx::createIndexBuffer(imem, BGFX_BUFFER_INDEX32);
```

`BGFX_BUFFER_INDEX32` is required — dense chunks exceed 65,535 vertices. If you want the
memory back, you can down-convert to 16-bit when `vertexCount < 65536`, which is most
chunks; measure before bothering.

Use `bgfx::makeRef` instead of `copy` only if you keep the file buffer alive until the
frame after submission; `copy` is the safe default for a streamer.

### 4.3 Reading a chunk

```cpp
bool loadChunk(const std::string& path, Chunk& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<char> buf((std::istreambuf_iterator<char>(f)), {});
    if (buf.size() < sizeof(ChunkHeader)) return false;

    const auto* h = reinterpret_cast<const ChunkHeader*>(buf.data());
    if (memcmp(h->magic, "CHNK", 4) != 0 || h->version != 1) return false;

    const char* p = buf.data() + sizeof(ChunkHeader);
    const auto* verts = reinterpret_cast<const Vertex*>(p);       p += h->vertexCount * 28;
    const auto* idx   = reinterpret_cast<const uint32_t*>(p);     p += h->indexCount * 4;
    const auto* binst = reinterpret_cast<const BuildingInst*>(p); p += h->buildingCount * 20;
    const auto* minst = reinterpret_cast<const MonumentInst*>(p);

    out.i = h->i; out.j = h->j;
    out.centre = { h->centreX, h->centreY };
    out.vbh = bgfx::createVertexBuffer(bgfx::copy(verts, h->vertexCount * 28), layout);
    out.ibh = bgfx::createIndexBuffer (bgfx::copy(idx,   h->indexCount  * 4 ), BGFX_BUFFER_INDEX32);
    out.buildings.assign(binst, binst + h->buildingCount);
    out.monuments.assign(minst, minst + h->monumentCount);
    return true;
}
```

Do the file read on a worker thread; only the two `create*Buffer` calls need the render
thread (or use `bgfx::Encoder` from a thread that owns it).

### 4.4 Drawing

Terrain is one call:

```cpp
float m[16];
bx::mtxTranslate(m, chunk.centre.x + shiftX, chunk.centre.y + shiftY, 0.0f);
bgfx::setTransform(m);
bgfx::setVertexBuffer(0, chunk.vbh);
bgfx::setIndexBuffer(chunk.ibh);
bgfx::setState(BGFX_STATE_DEFAULT);
bgfx::submit(viewId, terrainProgram);
```

Buildings are instanced, grouped by type. Sort each chunk's instances by `type` **once at
load**, and store the run offsets; then a chunk's entire building set is at most 20 draws:

```cpp
const uint16_t stride = 64;                       // one mat4 per instance
for (const TypeRun& run : chunk.runs) {
    if (bgfx::getAvailInstanceDataBuffer(run.count, stride) < run.count) break;
    bgfx::InstanceDataBuffer idb;
    bgfx::allocInstanceDataBuffer(&idb, run.count, stride);

    auto* data = reinterpret_cast<float*>(idb.data);
    for (uint32_t k = 0; k < run.count; ++k) {
        const BuildingInst& b = chunk.buildings[run.first + k];
        bx::mtxRotateZ(data, b.rot);
        data[12] = chunk.centre.x + shiftX + b.x;
        data[13] = chunk.centre.y + shiftY + b.y;
        data[14] = b.z;
        data += 16;
    }
    const Proto& proto = protos[run.type][lodLevel];
    bgfx::setVertexBuffer(0, proto.vbh);
    bgfx::setIndexBuffer(proto.ibh);
    bgfx::setInstanceDataBuffer(&idb);
    bgfx::setState(BGFX_STATE_DEFAULT);
    bgfx::submit(viewId, buildingProgram);
}
```

Check `bgfx::getCaps()->supported & BGFX_CAPS_INSTANCING` at startup and keep a plain
per-instance fallback loop; every desktop target supports it, but the fallback is ten lines.

If 64 bytes per instance is heavier than you like, pass 16 bytes (`x, y, z, rot`) as
`i_data0` and build the rotation in the vertex shader — a sin/cos per vertex is cheaper
than the bandwidth at these instance counts.

### 4.5 Culling bounds

Buildings are assigned to the chunk containing their centroid and are allowed to overhang,
by up to ~50 m for a large footprint and ~100 m for a large roundabout platform. **Pad
chunk bounding boxes by 110 m horizontally**, and by the tallest thing in the chunk
vertically (buildings reach 40 m, large monuments reach ~106 m). Tight bounds will pop
geometry at the edges.

---

## 5. Streaming and the wrapping seam

Chunk *identity* and chunk *placement* are separate. For a viewer in column `c`, place
columns `c-k … c+k` at `(c + offset) * 502` and resolve identity with `mod 50`. You never
render "column 49 next to column 0"; you render a continuous run around the viewer and let
identities wrap underneath. That is what makes the seam invisible.

```cpp
for (int dc = -k; dc <= k; ++dc) {
    int col   = ((c + dc) % 50 + 50) % 50;          // which file
    float px  = float(c + dc) * 502.0f;             // where to put it
    drawChunk(chunkCache.get(col, row), px, ...);
}
```

Combine with origin rebasing: when the player crosses a column boundary, shift every chunk
translation by ±502 rather than teleporting the player. float32 quantizes to about 4 mm at
32 km but under a millimetre within a few km of the origin, which is where rebasing keeps
you.

The backdrop cylinder rotates instead of translating: rebasing by *k* columns means
rotating it about the axis by `k × 7.2°`, or offsetting its U coordinate by `k × 0.02`.

---

## 6. Replacing the placeholders with real art

This is the part the format was designed around. **The instance table never changes.** The
generator's output — which type sits where, at what rotation — is the durable data. The
prototypes are an asset reference, and the current baked boxes are just the first
implementation of that reference.

### 6.1 The art contract

A replacement model for type *N* must honour these, and nothing else:

| Rule | Value |
|---|---|
| Local origin | midpoint of the street-facing edge, at ground level |
| Orientation | +X along the street, +Y into the block, +Z up |
| Footprint | must stay inside `building_types[N].footprint` (a CCW quad, in local space) |
| Height | `building_types[N].height` (20–40 m in 5 m steps) |
| Scale | none applied at runtime — model at true metres |
| Courtyard | if `courtyard` is true: a central void with ~10 m thick wings |
| Passage | if courtyard: a 2 m wide × 3 m tall opening at the centre of the front edge |

Balconies, cornices, roof detail and signage may overhang the footprint by a metre or so —
the generator leaves 3 m of curb between the building line and the street, and 4 m of alley
between back-to-back rows, so there is room. Anything that overhangs more will intersect a
neighbour, because buildings are placed with party walls touching.

The footprint quads are in the manifest, so you can import them into Blender as a modelling
guide:

```python
import json, bpy
types = json.load(open("world/manifest.json"))["building_types"]
for t in types:
    verts = [(x, y, 0.0) for x, y in t["footprint"]]
    me = bpy.data.meshes.new(t["name"]); me.from_pydata(verts, [], [[0,1,2,3]])
    ob = bpy.data.objects.new("guide_" + t["name"], me)
    bpy.context.scene.collection.objects.link(ob)
```

### 6.2 Where the assets live

Replace `prototypes.bin` with an index plus real asset files. Keep the same type IDs:

```jsonc
// world/prototypes.json
{
  "types": [
    { "type": 0, "name": "Cube",
      "lods": [
        { "lod": 0, "file": "models/cube_lod0.glb", "max_distance": 150 },
        { "lod": 1, "file": "models/cube_lod1.glb", "max_distance": 600 },
        { "lod": 2, "file": "models/cube_lod2.glb", "max_distance": 2500 },
        { "lod": 3, "file": "models/cube_lod3.glb", "max_distance": 1e9 }
      ],
      "variants": ["models/cube_a.glb", "models/cube_b.glb", "models/cube_c.glb"] }
  ]
}
```

Load with **cgltf** (single header, no dependencies, handles .glb) and build the same
vertex/index buffers. The loader changes; the streamer, the instancing and the chunk files
do not.

So yes — more LOD prototypes is exactly the mechanism. Suggested ladder for a 20–40 m
building viewed from up to a few km inside the cylinder:

| LOD | Budget | Content |
|---|---|---|
| 0 | 3–8k tris | full facade detail, courtyard interior, passage opening |
| 1 | 800–1500 | facade planes with normal-mapped detail, courtyard void kept |
| 2 | 100–300 | solid massing, no courtyard, roof shape preserved |
| 3 | 12 | the current box — or drop to the backdrop entirely |

LOD 2 filling in the courtyard is fine at distance and roughly halves the triangle count;
that is what the current LOD1 prism already does.

### 6.3 Variants without touching the data

Rows of identical buildings are the thing that will give the placeholder away. Pick a
variant per instance from a hash of its position, so it is stable across sessions and
across regenerations, and costs zero bytes on disk:

```cpp
inline uint32_t instanceHash(float x, float y) {
    uint32_t h = uint32_t(int32_t(x * 10.0f)) * 2654435761u
               ^ uint32_t(int32_t(y * 10.0f)) * 2246822519u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    return h;
}
// variant  = instanceHash(worldX, worldY) % variantCount;
// material = paletteCount ? (instanceHash(worldX, worldY) >> 8) % paletteCount : 0;
```

Use the same hash to vary facade tint, roof material and window sets. Instances must stay
grouped by *(type, variant)* for instancing, so make the variant part of the sort key at
load time — it's the same pass that already sorts by type.

### 6.4 Monuments

Three prototypes, same instancing path, keyed on `monument.size`:

- **S** (496 of them): gold cylinder, radius 1.5 m, height 5 m, standing on a 15 m platform
- **M** (272): gold cylinder, radius 3 m, height 10 m, on a 40 m platform
- **L** (32): off-white drum, radius 65 m, 40 m tall, capped by a 65 m hemisphere — about
  106 m overall, on a 70 m platform

The large ones are at least ~2.5 km apart by construction, so they are the natural
long-range landmark set for navigation. Each has a `Monument_S_###` / `_M_` / `_L_` socket
in the Blender scene if you want to hand-place hero art on specific ones; in the binary
they are just instances, and you can special-case individual positions in the engine.

Platform height is 1.5 m — monument models sit at `z = 1.5`, not at 0.

---

## 7. The backdrop

The far side of the shell is not streamed geometry. It is a single cylinder at radius
`3994.8 − 48 ≈ 3947 m` (the rooftop shell, so parallax displaces *away* from the viewer)
carrying:

- **colour plate** — orthographic plan render, `render_map.py`, exact metric framing so U
  wraps seamlessly. 8192 px across the 25.1 km width is ~3 m/px, which matches a 4K display
  at 8 km viewing distance.
- **height map** — `bake_height.py`, single-channel 16-bit, linear, `1.0 = 112 m`, imported
  as non-colour data. Feeds parallax occlusion mapping; invert to depth-below-reference
  (`1.0 - tex`) in the shader.

UVs are `u = x / 25100`, `v = y / 32000`, repeat in U, clamp in V. Flip U or the image if
the inward-facing normals mirror it. Sample the height with an explicit LOD — automatic mip
selection fights the raymarch at grazing angles.

POM restores interior parallax (streets read as recessed, courtyards as holes) but not
silhouettes; the skyline at the visible edge stays a flat cut, so let fog or a band of real
LOD2/LOD3 geometry cover the transition.

---

## 8. Build commands

```bash
# one region, for iterating
python3 export_chunks.py --region 20080 13500 25100 18500 --out world/

# everything, tile by tile (~1 hour, ~110 MB)
python3 export_chunks.py --all --tile 5020 4000 --out world/

# backdrop textures (needs Blender)
blender city.blend --background --python render_map.py  -- --atlas --ppm 0.32
blender city.blend --background --python bake_height.py -- \
        --frame 0 0 25100 32000 --ppm 0.32 --out //height.png

# whole-map overview in Blender, seconds, no buildings
blender --background --python preview_network.py -- --out //preview.blend
```

`SEED` in `paris_city.py` selects the world. Changing it invalidates everything — chunks,
backdrop, height map — so pin it in version control alongside the exported data.

---

## 9. Checklist

- [ ] Loader reads `manifest.json` and one chunk; `static_assert`s on struct sizes pass
- [ ] Terrain draws with the 28-byte layout and `BGFX_BUFFER_INDEX32`
- [ ] Prototypes load; one type draws instanced from one chunk
- [ ] Instances sorted by type at load, run offsets cached
- [ ] Chunk bounds padded 110 m horizontally, ~110 m vertically
- [ ] Column identity wraps `mod 50`; placement is relative to the viewer's column
- [ ] Origin rebases on column crossing; backdrop rotates `7.2°` per column
- [ ] All gameplay X differences go through `wrapDX`
- [ ] LOD distance table drives prototype selection per chunk
- [ ] Variant hash in the sort key, ready for when there is more than one model per type

---

## 10. Known rough edges

- **Tile seams.** Regions exported separately can leave a few overlapping buildings within
  ~100 m of a tile border (about 5 pairs per 1,000 buildings). Export with `pad ≈ 1500 m`
  and trim if that matters for playable areas; it is invisible in the backdrop bake.
- **Layered ground.** Ground, water, parks, roads and roundabouts are stacked 5 cm apart
  rather than cut out of a single surface. Fine for a blockout; replace with a real terrain
  mesh and a masked material when you get there, at which point the chunk terrain mesh
  becomes a heightfield plus splat maps instead of coloured triangles.
- **No collision data** is exported. Building footprints and heights are in the manifest and
  the instance table, so boxes are trivial to derive; road surfaces are flat at z ≈ 0.2–0.3
  except bridge decks at 6 m.
- **The river is water-flat**, not carved. Bridge decks ramp to 6 m over 80 m; there is no
  bank geometry below z = 0.
