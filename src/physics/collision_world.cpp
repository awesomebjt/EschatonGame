#include "physics/collision_world.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "world/world_loader.h"
#include "world/wrap.h"

namespace physics {

namespace {

constexpr double k_cell_target = 32.0;   // m; a capsule query touches 1–4 cells

int pack_chunk(int col, int row) { return col | (row << 16); }
int chunk_col(int packed) { return packed & 0xffff; }
int chunk_row(int packed) { return packed >> 16; }

} // namespace

bool CollisionWorld::build(const world::WorldData& w)
{
    const world::Manifest& m = w.manifest;
    m_map_w   = m.map_w;
    m_map_h   = m.map_h;
    m_chunk_x = m.chunk_x;
    m_chunk_y = m.chunk_y;
    m_columns = m.columns;

    m_cells_x = std::max(1, static_cast<int>(std::lround(m_map_w / k_cell_target)));
    m_cells_y = std::max(1, static_cast<int>(std::ceil(m_map_h / k_cell_target)));
    m_cell_w  = m_map_w / m_cells_x;   // exact division keeps the x wrap seamless
    m_cell_h  = m_map_h / m_cells_y;

    // -- Gather items ---------------------------------------------------------
    size_t tri_total = 0;
    for (const world::CollisionChunk& c : w.collision) tri_total += c.triangles.size() / 9;
    m_terrain.clear();
    m_terrain.reserve(tri_total * 9);
    m_terrain_chunk.clear();
    m_terrain_chunk.reserve(tri_total);
    for (const world::CollisionChunk& c : w.collision) {
        m_terrain.insert(m_terrain.end(), c.triangles.begin(), c.triangles.end());
        m_terrain_chunk.insert(m_terrain_chunk.end(), c.triangles.size() / 9, pack_chunk(c.i, c.j));
    }

    m_proto.assign(w.prototypes.size(), {});
    std::vector<float> proto_reach(w.prototypes.size(), 0.0f);
    for (size_t t = 0; t < w.prototypes.size(); ++t) {
        const world::PrototypeMesh& p = w.prototypes[t];
        for (uint32_t index : p.indices) {
            const float* pos = p.vertices[index].pos;
            m_proto[t].insert(m_proto[t].end(), pos, pos + 3);
            proto_reach[t] = std::max(proto_reach[t], std::hypot(pos[0], pos[1]));
        }
    }

    m_buildings.clear();
    for (size_t t = 0; t < w.buildings.size(); ++t) {
        for (const world::BuildingInstance& b : w.buildings[t]) {
            m_buildings.push_back({static_cast<uint16_t>(t), static_cast<int16_t>(b.col),
                                   static_cast<int16_t>(b.row), b.x, b.y, b.z, b.rot,
                                   std::cos(b.rot), std::sin(b.rot)});
        }
    }
    const size_t terrain_count = m_terrain_chunk.size();
    if (terrain_count + m_buildings.size() >= k_building_bit) {
        std::fprintf(stderr, "collision: too many items for the grid id space\n");
        return false;
    }

    // -- Bucket into the grid (count, then fill) -------------------------------
    // Each item's map-space xy bounds, as (x0, x1, y0, y1); x may run past either
    // end of the map, and the cell range wraps.
    auto terrain_bounds = [&](size_t t, double b[4]) {
        const int    pc = m_terrain_chunk[t];
        const double cx = (chunk_col(pc) + 0.5) * m_chunk_x;
        const double cy = (chunk_row(pc) + 0.5) * m_chunk_y;
        const float* v  = &m_terrain[t * 9];
        b[0] = cx + std::min({v[0], v[3], v[6]});
        b[1] = cx + std::max({v[0], v[3], v[6]});
        b[2] = cy + std::min({v[1], v[4], v[7]});
        b[3] = cy + std::max({v[1], v[4], v[7]});
    };
    auto building_bounds = [&](size_t k, double b[4]) {
        const Building& bd = m_buildings[k];
        const double cx = (bd.chunk_col + 0.5) * m_chunk_x + bd.x;
        const double cy = (bd.chunk_row + 0.5) * m_chunk_y + bd.y;
        const double r  = proto_reach[bd.type];
        b[0] = cx - r; b[1] = cx + r; b[2] = cy - r; b[3] = cy + r;
    };
    auto for_cells = [&](const double b[4], auto&& fn) {
        const int x0 = static_cast<int>(std::floor(b[0] / m_cell_w));
        const int x1 = std::min(x0 + m_cells_x - 1, static_cast<int>(std::floor(b[1] / m_cell_w)));
        const int y0 = std::clamp(static_cast<int>(std::floor(b[2] / m_cell_h)), 0, m_cells_y - 1);
        const int y1 = std::clamp(static_cast<int>(std::floor(b[3] / m_cell_h)), 0, m_cells_y - 1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                fn(static_cast<size_t>(y) * m_cells_x + ((x % m_cells_x) + m_cells_x) % m_cells_x);
    };

    const size_t cell_count = static_cast<size_t>(m_cells_x) * m_cells_y;
    std::vector<uint32_t> counts(cell_count + 1, 0);
    double b[4];
    for (size_t t = 0; t < terrain_count; ++t) {
        terrain_bounds(t, b);
        for_cells(b, [&](size_t c) { ++counts[c]; });
    }
    for (size_t k = 0; k < m_buildings.size(); ++k) {
        building_bounds(k, b);
        for_cells(b, [&](size_t c) { ++counts[c]; });
    }

    m_cell_start.assign(cell_count + 1, 0);
    for (size_t c = 0; c < cell_count; ++c) m_cell_start[c + 1] = m_cell_start[c] + counts[c];
    m_cells.assign(m_cell_start[cell_count], 0);
    std::vector<uint32_t> fill(m_cell_start.begin(), m_cell_start.end() - 1);
    for (size_t t = 0; t < terrain_count; ++t) {
        terrain_bounds(t, b);
        for_cells(b, [&](size_t c) { m_cells[fill[c]++] = static_cast<uint32_t>(t); });
    }
    for (size_t k = 0; k < m_buildings.size(); ++k) {
        building_bounds(k, b);
        for_cells(b, [&](size_t c) {
            m_cells[fill[c]++] = static_cast<uint32_t>(terrain_count + k) | k_building_bit;
        });
    }

    m_stamp.assign(terrain_count + m_buildings.size(), 0);
    m_query = 0;
    return true;
}

template <typename Fn>
void CollisionWorld::for_each_in_box(double x, double y, float half, Fn&& fn) const
{
    if (++m_query == 0) {   // stamp wrapped: clear so stale stamps can't match
        std::fill(m_stamp.begin(), m_stamp.end(), 0);
        m_query = 1;
    }
    const int x0 = static_cast<int>(std::floor((x - half) / m_cell_w));
    const int x1 = std::min(x0 + m_cells_x - 1, static_cast<int>(std::floor((x + half) / m_cell_w)));
    const int y0 = std::clamp(static_cast<int>(std::floor((y - half) / m_cell_h)), 0, m_cells_y - 1);
    const int y1 = std::clamp(static_cast<int>(std::floor((y + half) / m_cell_h)), 0, m_cells_y - 1);
    const size_t terrain_count = m_terrain_chunk.size();
    for (int cy = y0; cy <= y1; ++cy) {
        for (int cx = x0; cx <= x1; ++cx) {
            const size_t c = static_cast<size_t>(cy) * m_cells_x + ((cx % m_cells_x) + m_cells_x) % m_cells_x;
            for (uint32_t k = m_cell_start[c]; k < m_cell_start[c + 1]; ++k) {
                const uint32_t id   = m_cells[k] & ~k_building_bit;
                if (m_stamp[id] == m_query) continue;
                m_stamp[id] = m_query;
                if (m_cells[k] & k_building_bit) fn(false, id - terrain_count);
                else                            fn(true, id);
            }
        }
    }
}

void CollisionWorld::gather(double ox, double oy, double oz, float half, std::vector<Tri>& out) const
{
    if (empty()) return;
    for_each_in_box(ox, oy, half, [&](bool terrain, size_t id) {
        if (terrain) {
            const int    pc = m_terrain_chunk[id];
            const float  dx = static_cast<float>(world::wrap_dx((chunk_col(pc) + 0.5) * m_chunk_x - ox));
            const float  dy = static_cast<float>((chunk_row(pc) + 0.5) * m_chunk_y - oy);
            const float  dz = static_cast<float>(-oz);
            const float* v  = &m_terrain[id * 9];
            Tri t{{v[0] + dx, v[1] + dy, v[2] + dz}, {v[3] + dx, v[4] + dy, v[5] + dz},
                  {v[6] + dx, v[7] + dy, v[8] + dz}};
            // Cells are coarse; skip triangles that miss the box in xy.
            if (std::max({t.a[0], t.b[0], t.c[0]}) < -half || std::min({t.a[0], t.b[0], t.c[0]}) > half
                || std::max({t.a[1], t.b[1], t.c[1]}) < -half || std::min({t.a[1], t.b[1], t.c[1]}) > half)
                return;
            out.push_back(t);
            return;
        }
        const Building&           bd = m_buildings[id];
        const std::vector<float>& p  = m_proto[bd.type];
        const float dx = static_cast<float>(world::wrap_dx((bd.chunk_col + 0.5) * m_chunk_x + bd.x - ox));
        const float dy = static_cast<float>((bd.chunk_row + 0.5) * m_chunk_y + bd.y - oy);
        const float dz = static_cast<float>(bd.z - oz);
        for (size_t k = 0; k + 9 <= p.size(); k += 9) {
            Tri t;
            float* dst[3] = {t.a, t.b, t.c};
            for (int v = 0; v < 3; ++v) {
                const float* s = &p[k + v * 3];
                dst[v][0] = bd.cos_r * s[0] - bd.sin_r * s[1] + dx;
                dst[v][1] = bd.sin_r * s[0] + bd.cos_r * s[1] + dy;
                dst[v][2] = s[2] + dz;
            }
            if (std::max({t.a[0], t.b[0], t.c[0]}) < -half || std::min({t.a[0], t.b[0], t.c[0]}) > half
                || std::max({t.a[1], t.b[1], t.c[1]}) < -half || std::min({t.a[1], t.b[1], t.c[1]}) > half)
                continue;
            out.push_back(t);
        }
    });
}

std::optional<RayHit> CollisionWorld::raycast_down(double x, double y, double from_alt) const
{
    if (empty()) return std::nullopt;
    std::optional<RayHit> best;

    auto test = [&](const Tri& t, bool building) {
        // Vertical line through the origin: 2D point-in-triangle, then the plane's z.
        const float e0x = t.b[0] - t.a[0], e0y = t.b[1] - t.a[1];
        const float e1x = t.c[0] - t.a[0], e1y = t.c[1] - t.a[1];
        const float det = e0x * e1y - e0y * e1x;
        if (std::fabs(det) < 1e-6f) return;   // vertical face
        const float px = -t.a[0], py = -t.a[1];
        const float u  = (px * e1y - py * e1x) / det;
        const float v  = (e0x * py - e0y * px) / det;
        if (u < 0 || v < 0 || u + v > 1) return;
        const double z = t.a[2] + u * (t.b[2] - t.a[2]) + v * (t.c[2] - t.a[2]);
        if (z > from_alt + 1e-3) return;
        if (!best || z > best->alt) best = RayHit{z, building};
    };

    for_each_in_box(x, y, 0.01f, [&](bool terrain, size_t id) {
        if (terrain) {
            const int    pc = m_terrain_chunk[id];
            const float  dx = static_cast<float>(world::wrap_dx((chunk_col(pc) + 0.5) * m_chunk_x - x));
            const float  dy = static_cast<float>((chunk_row(pc) + 0.5) * m_chunk_y - y);
            const float* v  = &m_terrain[id * 9];
            test(Tri{{v[0] + dx, v[1] + dy, v[2]}, {v[3] + dx, v[4] + dy, v[5]}, {v[6] + dx, v[7] + dy, v[8]}},
                 false);
            return;
        }
        const Building&           bd = m_buildings[id];
        const std::vector<float>& p  = m_proto[bd.type];
        const float dx = static_cast<float>(world::wrap_dx((bd.chunk_col + 0.5) * m_chunk_x + bd.x - x));
        const float dy = static_cast<float>((bd.chunk_row + 0.5) * m_chunk_y + bd.y - y);
        for (size_t k = 0; k + 9 <= p.size(); k += 9) {
            Tri t;
            float* dst[3] = {t.a, t.b, t.c};
            for (int v = 0; v < 3; ++v) {
                const float* s = &p[k + v * 3];
                dst[v][0] = bd.cos_r * s[0] - bd.sin_r * s[1] + dx;
                dst[v][1] = bd.sin_r * s[0] + bd.cos_r * s[1] + dy;
                dst[v][2] = s[2] + bd.z;
            }
            test(t, true);
        }
    });
    return best;
}

} // namespace physics
