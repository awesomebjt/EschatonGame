#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Binary chunk / prototype formats written by tools/export_chunks.py.
// Byte-level reference: docs/WORLD_PIPELINE.md.
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
#pragma pack(pop)

static_assert(sizeof(ChunkHeader) == 40);
static_assert(sizeof(Vertex) == 28);
static_assert(sizeof(BuildingInst) == 20);
static_assert(sizeof(MonumentInst) == 16);

struct ChunkData
{
    ChunkHeader               header{};
    std::vector<Vertex>       vertices;
    std::vector<uint32_t>     indices;
    std::vector<BuildingInst> buildings;
    std::vector<MonumentInst> monuments;
};

struct PrototypeMesh
{
    uint16_t              type = 0;
    uint16_t              lod  = 0;
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
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
    std::vector<ChunkEntry>   chunks;
};

std::optional<Manifest>                   load_manifest(const std::string& path);
std::optional<ChunkData>                  load_chunk(const std::string& path);
std::optional<std::vector<PrototypeMesh>> load_prototypes(const std::string& path);

} // namespace world
