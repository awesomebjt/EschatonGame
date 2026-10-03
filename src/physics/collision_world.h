#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace world { struct WorldData; }

namespace physics {

// A triangle relative to some query origin, in local map axes (x around, y along,
// z up). Small numbers only: callers do all contact maths in this frame.
struct Tri
{
    float a[3], b[3], c[3];
};

struct RayHit
{
    double alt      = 0;       // map altitude of the hit
    bool   building = false;   // hit a building rather than terrain
};

// Static collision geometry for a whole cylinder, in flat map space: the
// exporter's terrain triangles (roads, platforms, bridge decks, monuments) and
// every building instance tested against its prototype's LOD0 mesh, so courtyards
// and passages stay open. Flat space is close enough: next to a wall the rendered
// (curved, rigid-building) world differs from it by about a millimetre.
//
// A uniform grid over the map, wrapping in x, lists which terrain triangles and
// building instances overlap each cell.
class CollisionWorld
{
public:
    bool build(const world::WorldData& world);
    bool empty() const { return m_cells.empty(); }

    // Appends every triangle whose cell overlaps the box [x ± half, y ± half] to
    // `out`, expressed relative to (ox, oy, oz). The box is in map metres around
    // the origin; x wraps.
    void gather(double ox, double oy, double oz, float half, std::vector<Tri>& out) const;

    // Highest surface at or below `from_alt` under map point (x, y).
    std::optional<RayHit> raycast_down(double x, double y, double from_alt) const;

private:
    struct Building
    {
        uint16_t type;
        int16_t  chunk_col, chunk_row;
        float    x, y, z, rot;   // relative to the chunk centre
        float    cos_r, sin_r;
    };

    template <typename Fn> void for_each_in_box(double x, double y, float half, Fn&& fn) const;

    double m_map_w = 0, m_map_h = 0, m_chunk_x = 0, m_chunk_y = 0;
    int    m_columns = 0;

    // Terrain: 9 floats per triangle relative to its chunk centre, plus the chunk.
    std::vector<float>   m_terrain;
    std::vector<int32_t> m_terrain_chunk;   // packed col | row << 16

    std::vector<Building>           m_buildings;
    std::vector<std::vector<float>> m_proto;   // per type: 9 floats per triangle, local

    // Grid: CSR lists of item ids; ids >= k_building_bit are buildings.
    static constexpr uint32_t k_building_bit = 0x80000000u;
    double                m_cell_w = 0, m_cell_h = 0;
    int                   m_cells_x = 0, m_cells_y = 0;
    std::vector<uint32_t> m_cell_start;   // m_cells_x * m_cells_y + 1
    std::vector<uint32_t> m_cells;

    mutable std::vector<uint32_t> m_stamp;   // per item: last query that visited it
    mutable uint32_t              m_query = 0;
};

} // namespace physics
