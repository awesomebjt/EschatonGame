#include "world/world_loader.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>

namespace world {

namespace {

// Chunks per terrain group: 5 × 4 chunks ≈ 2.5 × 2 km, 160 groups for the cylinder.
constexpr int k_group_cols = 5;
constexpr int k_group_rows = 4;

// A chord of arc length L sags L² / 8R below the cylinder: 25 m → 2 cm at R ≈ 4 km.
constexpr double k_max_chord = 25.0;

Vertex lerp_at_x(const Vertex& p, const Vertex& q, float x)
{
    const float t = (x - p.pos[0]) / (q.pos[0] - p.pos[0]);
    Vertex v = p;   // faces are flat-shaded: normal and colour are per face
    v.pos[0] = x;
    v.pos[1] = p.pos[1] + (q.pos[1] - p.pos[1]) * t;
    v.pos[2] = p.pos[2] + (q.pos[2] - p.pos[2]) * t;
    return v;
}

// Sutherland–Hodgman against one vertical line; `keep_above` keeps x >= line.
void clip_x(const std::vector<Vertex>& in, float line, bool keep_above, std::vector<Vertex>& out)
{
    out.clear();
    const size_t n = in.size();
    for (size_t k = 0; k < n; ++k) {
        const Vertex& p = in[k];
        const Vertex& q = in[(k + 1) % n];
        const bool p_in = keep_above ? p.pos[0] >= line : p.pos[0] <= line;
        const bool q_in = keep_above ? q.pos[0] >= line : q.pos[0] <= line;
        if (p_in) out.push_back(p);
        if (p_in != q_in) out.push_back(lerp_at_x(p, q, line));
    }
}

void emit_fan(const std::vector<Vertex>& poly, float ox, float oy, std::vector<Vertex>& vout,
              std::vector<uint32_t>& iout)
{
    if (poly.size() < 3) return;
    const uint32_t base = static_cast<uint32_t>(vout.size());
    for (Vertex v : poly) {
        v.pos[0] += ox;
        v.pos[1] += oy;
        vout.push_back(v);
    }
    for (uint32_t k = 1; k + 1 < poly.size(); ++k) {
        iout.push_back(base);
        iout.push_back(base + k);
        iout.push_back(base + k + 1);
    }
}

// Deterministic [0, 1) from a building's chunk and slot, for per-instance tint.
float hash01(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = a * 0x9E3779B1u ^ b * 0x85EBCA77u ^ c * 0xC2B2AE3Du;
    h ^= h >> 15; h *= 0x2C1B3C6Du;
    h ^= h >> 12; h *= 0x297A2D39u;
    h ^= h >> 15;
    return static_cast<float>(h >> 8) / static_cast<float>(1u << 24);
}

} // namespace

void cut_circumferential(const ChunkData& chunk, double cut_origin, double step, float ox, float oy,
                         std::vector<Vertex>& vout, std::vector<uint32_t>& iout)
{
    const std::vector<Vertex>&   v   = chunk.vertices;
    const std::vector<uint32_t>& idx = chunk.indices;
    const size_t tri_count = idx.size() / 3;

    std::vector<Vertex> poly, slab, tmp;
    size_t t = 0;
    while (t < tri_count) {
        // The exporter fan-triangulates each convex face as (base, base+k, base+k+1);
        // regrouping the fan recovers the face so it can be clipped as a polygon.
        const uint32_t a = idx[t * 3];
        poly.assign({v[a], v[idx[t * 3 + 1]], v[idx[t * 3 + 2]]});
        uint32_t last = idx[t * 3 + 2];
        ++t;
        while (t < tri_count && idx[t * 3] == a && idx[t * 3 + 1] == last) {
            last = idx[t * 3 + 2];
            poly.push_back(v[last]);
            ++t;
        }

        float xmin = poly[0].pos[0], xmax = xmin;
        for (const Vertex& p : poly) {
            xmin = std::min(xmin, p.pos[0]);
            xmax = std::max(xmax, p.pos[0]);
        }

        // Lines strictly inside the face; a face touching a line at an edge is not cut.
        constexpr double eps = 1e-3;
        const double k0 = std::floor((xmin + eps - cut_origin) / step) + 1.0;
        const double k1 = std::ceil((xmax - eps - cut_origin) / step) - 1.0;
        if (k1 < k0) {
            emit_fan(poly, ox, oy, vout, iout);
            continue;
        }

        float lo = -INFINITY;
        for (double k = k0; k <= k1 + 0.5; k += 1.0) {
            const float hi = static_cast<float>(cut_origin + k * step);
            clip_x(poly, hi, /*keep_above=*/false, tmp);
            if (std::isfinite(lo)) clip_x(tmp, lo, /*keep_above=*/true, slab);
            else slab.swap(tmp);
            emit_fan(slab, ox, oy, vout, iout);
            lo = hi;
        }
        clip_x(poly, lo, /*keep_above=*/true, slab);
        emit_fan(slab, ox, oy, vout, iout);
    }
}

std::optional<WorldData> load_world(const std::string& dir)
{
    auto manifest = load_manifest(dir + "/manifest.json");
    if (!manifest) return std::nullopt;

    WorldData w;
    w.manifest = std::move(*manifest);
    const Manifest& m = w.manifest;

    auto protos = load_prototypes(dir + "/prototypes.bin");
    if (!protos) return std::nullopt;
    const size_t type_count = m.building_types.size();
    w.prototypes.resize(type_count);
    for (PrototypeMesh& p : *protos)
        if (p.lod == 0 && p.type < type_count) w.prototypes[p.type] = std::move(p);
    for (size_t t = 0; t < type_count; ++t) {
        if (w.prototypes[t].indices.empty()) {
            std::fprintf(stderr, "world: prototypes.bin has no LOD0 mesh for type %zu\n", t);
            return std::nullopt;
        }
    }

    // Bucket the manifest's chunk list into groups.
    const int gcols = (m.columns + k_group_cols - 1) / k_group_cols;
    const int grows = (m.rows + k_group_rows - 1) / k_group_rows;
    std::vector<std::vector<const ChunkEntry*>> members(static_cast<size_t>(gcols * grows));
    for (const ChunkEntry& c : m.chunks)
        members[static_cast<size_t>((c.j / k_group_rows) * gcols + c.i / k_group_cols)].push_back(&c);

    w.groups.resize(members.size());
    for (int gj = 0; gj < grows; ++gj) {
        for (int gi = 0; gi < gcols; ++gi) {
            const int c0 = gi * k_group_cols, c1 = std::min(m.columns, c0 + k_group_cols);
            const int r0 = gj * k_group_rows, r1 = std::min(m.rows, r0 + k_group_rows);
            TerrainGroup& g = w.groups[static_cast<size_t>(gj * gcols + gi)];
            g.centre_x = 0.5 * (c0 + c1) * m.chunk_x;
            g.centre_y = 0.5 * (r0 + r1) * m.chunk_y;
        }
    }

    const double step = m.chunk_x / std::ceil(m.chunk_x / k_max_chord);
    const double cut_origin = -0.5 * m.chunk_x;   // a chunk's left edge, in chunk-local X

    std::vector<std::vector<std::vector<BuildingInstance>>> group_buildings(
        members.size(), std::vector<std::vector<BuildingInstance>>(type_count));
    std::vector<size_t> group_monuments(members.size(), 0);
    std::atomic<size_t> next{0};
    std::atomic<bool>   failed{false};

    auto worker = [&] {
        for (size_t gidx; (gidx = next.fetch_add(1)) < members.size() && !failed;) {
            TerrainGroup& g = w.groups[gidx];
            for (const ChunkEntry* e : members[gidx]) {
                auto chunk = load_chunk(dir + "/" + e->file);
                if (!chunk || chunk->header.i != e->i || chunk->header.j != e->j) {
                    if (chunk) std::fprintf(stderr, "world: %s header disagrees with manifest\n",
                                            e->file.c_str());
                    failed = true;
                    return;
                }
                const double cx = (e->i + 0.5) * m.chunk_x;
                const double cy = (e->j + 0.5) * m.chunk_y;
                cut_circumferential(*chunk, cut_origin, step,
                                    static_cast<float>(cx - g.centre_x),
                                    static_cast<float>(cy - g.centre_y), g.vertices, g.indices);

                for (size_t k = 0; k < chunk->buildings.size(); ++k) {
                    const BuildingInst& b = chunk->buildings[k];
                    if (b.type >= type_count) {
                        std::fprintf(stderr, "world: %s has unknown building type %u\n",
                                     e->file.c_str(), b.type);
                        failed = true;
                        return;
                    }
                    group_buildings[gidx][b.type].push_back(
                        {b.x, b.y, b.z, b.rot, static_cast<float>(e->i), static_cast<float>(e->j),
                         hash01(static_cast<uint32_t>(e->i), static_cast<uint32_t>(e->j),
                                static_cast<uint32_t>(k)),
                         0.0f});
                }
                // Monuments are already baked into the terrain mesh; the table only
                // matters for scripted landmarks later.
                group_monuments[gidx] += chunk->monuments.size();
            }
        }
    };

    const unsigned thread_count = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> threads;
    for (unsigned k = 0; k < thread_count; ++k) threads.emplace_back(worker);
    for (std::thread& th : threads) th.join();
    if (failed) return std::nullopt;

    // Merge in group order so the result is deterministic regardless of scheduling.
    w.buildings.resize(type_count);
    for (size_t gidx = 0; gidx < members.size(); ++gidx) {
        for (size_t t = 0; t < type_count; ++t) {
            auto& src = group_buildings[gidx][t];
            w.buildings[t].insert(w.buildings[t].end(), src.begin(), src.end());
        }
        w.monument_count += group_monuments[gidx];
    }
    return w;
}

} // namespace world
