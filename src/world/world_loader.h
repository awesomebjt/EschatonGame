#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "world/formats.h"

namespace world {

// Several chunks' terrain merged into one mesh, so the whole cylinder draws in a
// few hundred calls instead of 3,200. Vertices are relative to the group centre;
// groups are a few km across at most, which float32 holds to well under a mm.
struct TerrainGroup
{
    double                centre_x = 0, centre_y = 0;   // map space
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
};

// GPU instance record (two vec4s). Position stays chunk-relative and carries the
// chunk index, so the shader can rebuild a camera-relative offset without ever
// holding an absolute 25 km coordinate in float.
struct BuildingInstance
{
    float x, y, z, rot;           // relative to the owning chunk's centre
    float col, row, shade, pad;   // chunk indices; per-instance tint seed in [0, 1)
};
static_assert(sizeof(BuildingInstance) == 32);

// A chunk's terrain as the exporter wrote it, before the render-side cuts: flat
// map space is what collision runs in, so the curvature subdivision isn't needed.
struct CollisionChunk
{
    int                i = 0, j = 0;
    std::vector<float> triangles;   // 9 floats per triangle, relative to the chunk centre
};

struct WorldData
{
    Manifest                                   manifest;
    std::vector<TerrainGroup>                  groups;
    std::vector<std::vector<BuildingInstance>> buildings;    // indexed by building type
    std::vector<PrototypeMesh>                 prototypes;   // LOD0, indexed by building type
    std::vector<CollisionChunk>                collision;    // one per manifest chunk, same order
    size_t                                     monument_count = 0;
};

// Loads every chunk listed in `dir`/manifest.json, subdivides terrain for the
// cylindrical projection, and merges it into render groups. Multithreaded.
std::optional<WorldData> load_world(const std::string& dir);

// Cuts every terrain polygon along lines of constant map X spaced `step` apart
// (`cut_origin` is any one line, in the same coordinates as the vertices), so no
// flat face spans more than `step` of arc once wrapped onto the cylinder. Lines
// are shared by every chunk, so neighbouring faces split at identical points and
// stacked layers stay parallel within each strip. Output positions are shifted by
// (ox, oy) and appended to `vout` / `iout`.
void cut_circumferential(const ChunkData& chunk, double cut_origin, double step, float ox, float oy,
                         std::vector<Vertex>& vout, std::vector<uint32_t>& iout);

} // namespace world
