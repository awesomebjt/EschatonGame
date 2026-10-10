#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Binary chunk formats written by tools/export_chunks.py, and the building
// prototypes written by tools/prototypes.py. Byte-level reference:
// docs/WORLD_PIPELINE.md and tools/PROTOTYPES_INTEGRATION.md.
namespace world {

#pragma pack(push, 1)
struct ChunkHeader                         // 40 bytes, magic "CHNK", version 1
{
    char     magic[4];
    uint32_t version;
    int32_t  i, j;
    float    centre_x, centre_y;
    uint32_t vertex_count, index_count, building_count, monument_count;
};

struct Vertex                              // maps 1:1 onto the bgfx terrain layout
{
    float   pos[3];
    float   nrm[3];
    uint8_t rgba[4];
};

struct BuildingInst                        // chunk-local, placed as translate · rotateZ(rot)
{
    uint16_t type;
    uint16_t lod_hint;
    float    x, y, z, rot;
};

struct MonumentInst
{
    uint16_t size;                         // 0 = S, 1 = M, 2 = L
    uint16_t pad;
    float    x, y, z;
};

// Building prototype vertex (prototypes.bin v2), its own bgfx layout. UVs are
// metric: u along the wall, phased so bay centres sit at u = bay/2 (mod bay), and
// v above the building base. Tangent is the wall direction; w is always +1.
struct BuildVertex
{
    float pos[3];
    float nrm[3];
    float tan[4];
    float uv[2];
};
#pragma pack(pop)

static_assert(sizeof(ChunkHeader) == 40);
static_assert(sizeof(Vertex) == 28);
static_assert(sizeof(BuildingInst) == 20);
static_assert(sizeof(MonumentInst) == 16);
static_assert(sizeof(BuildVertex) == 48);

struct ChunkData
{
    ChunkHeader               header{};
    std::vector<Vertex>       vertices;
    std::vector<uint32_t>     indices;
    std::vector<BuildingInst> buildings;
    std::vector<MonumentInst> monuments;
};

// Which piece of a building a prototype mesh is. Collision is the solid massing
// (courtyard and passage open), in its own file; physics never sees the detail shell.
enum ProtoPart : uint16_t
{
    k_part_exterior     = 0,
    k_part_glazing      = 1,   // a pane in every window, drawn with interior mapping
    k_part_interior     = 2,   // rooms and slabs, LOD0 only, drawn near the player
    k_part_collision    = 3,
    k_part_door_glazing = 4,   // street doors only; dropped where the interior is drawn
    k_part_count
};

struct PrototypeMesh
{
    uint16_t                 type = 0;
    uint16_t                 lod  = 0;
    uint16_t                 part = 0;
    std::vector<BuildVertex> vertices;
    std::vector<uint32_t>    indices;

    bool empty() const { return indices.empty(); }
};

struct LodBand
{
    float max_distance = 0;   // metres; the band runs from the previous band's max
    bool  openings     = false;   // real window openings, so glazing is drawn
};

// prototypes.json: everything about the prototypes that isn't mesh data. The
// per-type building plans (walls, bays, doors, floors) are in the file too but
// not parsed yet; nothing consumes them.
struct PrototypeIndex
{
    std::vector<LodBand> lod_bands;
    float                interior_max_distance = 0;
    // Facade tile, for the material that redraws windows on walls without openings.
    // Windows are centred in the bay, with a semicircular head of half their width.
    float bay = 0;          // tile size; UVs repeat every `bay` metres
    float window_w = 0, window_h = 0, window_sill = 0;   // height includes the arch
    float recess = 0;       // how deep the LOD0 openings are
    std::string          shell_file, interior_file, collision_file;
};

struct BuildingType
{
    std::string name;
    float       height    = 0.0f;
    bool        courtyard = false;
};

struct ChunkEntry
{
    int         i = 0, j = 0;
    std::string file;
};

struct Manifest
{
    double map_w = 0, map_h = 0;
    double chunk_x = 0, chunk_y = 0;
    double radius = 0;
    int    columns = 0, rows = 0;

    std::vector<BuildingType> building_types;
    float                     building_rgb[3] = {1, 1, 1};   // the building material
    std::vector<ChunkEntry>   chunks;
};

std::optional<Manifest>                   load_manifest(const std::string& path);
std::optional<ChunkData>                  load_chunk(const std::string& path);
std::optional<PrototypeIndex>             load_prototype_index(const std::string& path);
std::optional<std::vector<PrototypeMesh>> load_prototypes(const std::string& path);

} // namespace world
