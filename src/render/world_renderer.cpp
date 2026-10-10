#include "render/world_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "render/shader.h"
#include "world/world_loader.h"
#include "world/wrap.h"

namespace render {

namespace {

// Light column: a plain emissive tube on the axis. Not in the manifest because the
// generator doesn't model it; promote it to data when the column gets real design.
constexpr double   k_column_radius   = 30.0;
constexpr int      k_column_segments = 64;
constexpr uint8_t  k_column_rgb[3]   = {255, 246, 222};

bgfx::VertexLayout terrain_layout()
{
    bgfx::VertexLayout l;
    l.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal,   3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0,   4, bgfx::AttribType::Uint8, true)
        .end();
    return l;
}

bgfx::VertexLayout building_layout()
{
    bgfx::VertexLayout l;
    l.begin()
        .add(bgfx::Attrib::Position,  3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal,    3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Tangent,   4, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
    return l;
}

bgfx::VertexLayout instance_layout()
{
    bgfx::VertexLayout l;
    l.begin()
        .add(bgfx::Attrib::TexCoord7, 4, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord6, 4, bgfx::AttribType::Float)
        .end();
    return l;
}

template <typename T>
const bgfx::Memory* copy_vector(const std::vector<T>& v)
{
    return bgfx::copy(v.data(), static_cast<uint32_t>(v.size() * sizeof(T)));
}

// Facade modes for fs_building, in u_material.y.
constexpr float k_mode_masonry  = 0;   // walls with real openings: plain masonry
constexpr float k_mode_parallax = 1;   // no openings: windows redrawn, recessed by parallax
constexpr float k_mode_flat     = 2;   // no openings: windows redrawn flat
constexpr float k_mode_glazing  = 3;   // window panes: interior mapping
constexpr float k_mode_interior = 4;   // rooms

// LOD thresholds move this far either side of the band edge, so a building on
// the boundary doesn't flip LOD as the player paces back and forth.
constexpr float k_lod_hysteresis = 0.10f;

template <typename H>
void destroy_handle(H& h)
{
    if (bgfx::isValid(h)) bgfx::destroy(h);
    h = BGFX_INVALID_HANDLE;
}

} // namespace

bool WorldRenderer::create(const world::WorldData& w)
{
    const world::Manifest& m = w.manifest;
    m_radius  = m.radius;
    m_map_w   = m.map_w;
    m_map_h   = m.map_h;
    m_chunk_x = m.chunk_x;
    m_chunk_y = m.chunk_y;
    m_columns = m.columns;
    m_rows    = m.rows;
    m_stats   = {};

    m_terrain_prog  = load_program("vs_terrain.sc", "fs_world.sc");
    m_building_prog = load_program("vs_building.sc", "fs_building.sc");
    if (!bgfx::isValid(m_terrain_prog) || !bgfx::isValid(m_building_prog)) {
        destroy();
        return false;
    }

    u_cylinder  = bgfx::createUniform("u_cylinder",  bgfx::UniformType::Vec4);
    u_offset    = bgfx::createUniform("u_offset",    bgfx::UniformType::Vec4);
    u_cam_chunk = bgfx::createUniform("u_camChunk",  bgfx::UniformType::Vec4);
    u_grid      = bgfx::createUniform("u_grid",      bgfx::UniformType::Vec4);
    u_material  = bgfx::createUniform("u_material",  bgfx::UniformType::Vec4);
    u_fog       = bgfx::createUniform("u_fog",       bgfx::UniformType::Vec4);
    u_fog_color = bgfx::createUniform("u_fogColor",  bgfx::UniformType::Vec4);
    u_shadow_uv    = bgfx::createUniform("u_shadowUv",    bgfx::UniformType::Vec4);
    u_cloud_shadow = bgfx::createUniform("u_cloudShadow", bgfx::UniformType::Vec4);
    s_cloud_shadow = bgfx::createUniform("s_cloudShadow", bgfx::UniformType::Sampler);
    u_ring         = bgfx::createUniform("u_ring",        bgfx::UniformType::Vec4);
    u_facade       = bgfx::createUniform("u_facade",      bgfx::UniformType::Vec4);
    u_facade_color = bgfx::createUniform("u_facadeColor", bgfx::UniformType::Vec4);
    {
        const uint8_t zero = 0;
        m_no_shadow = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::R8, 0, bgfx::copy(&zero, 1));
    }

    const bgfx::VertexLayout layout = terrain_layout();

    for (const world::TerrainGroup& g : w.groups) {
        if (g.indices.empty()) continue;
        Group out;
        out.centre_x = g.centre_x;
        out.centre_y = g.centre_y;
        out.vb = bgfx::createVertexBuffer(copy_vector(g.vertices), layout);
        out.ib = bgfx::createIndexBuffer(copy_vector(g.indices), BGFX_BUFFER_INDEX32);
        m_groups.push_back(out);
        m_stats.terrain_triangles += g.indices.size() / 3;
    }
    m_stats.terrain_draws = static_cast<uint32_t>(m_groups.size()) + 1;   // + light column

    // -- Buildings --------------------------------------------------------------
    // Instancing needs no caps check: bgfx's renderer baseline guarantees it on
    // every backend (BGFX_CAPS_INSTANCING was removed upstream for that reason).
    const world::PrototypeIndex& pi = w.proto_index;
    m_lod_max.clear();
    m_lod_openings.clear();
    for (const world::LodBand& b : pi.lod_bands) {
        m_lod_max.push_back(b.max_distance);
        m_lod_openings.push_back(b.openings);
    }
    m_interior_max = pi.interior_max_distance;
    m_facade       = {pi.bay, pi.window_w, pi.window_h, pi.window_sill};
    m_facade_color = {m.building_rgb[0], m.building_rgb[1], m.building_rgb[2], pi.recess};
    // The near ring must reach past every band but the last (plus hysteresis);
    // beyond it everything is the last LOD anyway.
    const float ring_reach =
        m_lod_max.size() > 1 ? m_lod_max[m_lod_max.size() - 2] * (1 + k_lod_hysteresis) : 0;
    m_ring = static_cast<int>(std::ceil(ring_reach / std::min(m_chunk_x, m_chunk_y)));
    m_ring = std::min(m_ring, (m_columns - 1) / 2);   // never wrap onto itself

    {
        const bgfx::VertexLayout blayout     = building_layout();
        const bgfx::VertexLayout inst_layout = instance_layout();
        const size_t             lod_count   = m_lod_max.size();
        for (size_t t = 0; t < w.buildings.size(); ++t) {
            BuildingType bt;
            bt.lods.resize(lod_count);
            for (size_t l = 0; l < lod_count; ++l) {
                for (uint16_t part = 0; part < world::k_part_count; ++part) {
                    const world::PrototypeMesh& p = w.prototypes[t].lods[l][part];
                    if (p.empty()) continue;
                    Mesh& out     = bt.lods[l][part];
                    out.vb        = bgfx::createVertexBuffer(copy_vector(p.vertices), blayout);
                    out.ib        = bgfx::createIndexBuffer(copy_vector(p.indices), BGFX_BUFFER_INDEX32);
                    out.triangles = static_cast<uint32_t>(p.indices.size() / 3);
                }
            }
            bt.count = static_cast<uint32_t>(w.buildings[t].size());
            if (bt.count) {
                bt.instances = bgfx::createVertexBuffer(copy_vector(w.buildings[t]), inst_layout);
                m_stats.building_instances += bt.count;
                ++m_stats.building_draws;   // the far draw; submit() adds the near ring's
            }
            m_types.push_back(std::move(bt));
        }
    }

    // Near-ring index: each chunk's instances, by (type, slot).
    m_instances = w.buildings;
    const size_t chunk_count = static_cast<size_t>(m_columns) * m_rows;
    m_chunk_start.assign(chunk_count + 1, 0);
    auto chunk_of = [&](const world::BuildingInstance& b) {
        return static_cast<size_t>(b.row) * m_columns + static_cast<size_t>(b.col);
    };
    for (const auto& list : m_instances)
        for (const world::BuildingInstance& b : list) ++m_chunk_start[chunk_of(b) + 1];
    for (size_t c = 0; c < chunk_count; ++c) m_chunk_start[c + 1] += m_chunk_start[c];
    m_chunk_refs.resize(m_chunk_start[chunk_count]);
    {
        std::vector<uint32_t> fill(m_chunk_start.begin(), m_chunk_start.end() - 1);
        for (uint32_t t = 0; t < m_instances.size(); ++t)
            for (uint32_t k = 0; k < m_instances[t].size(); ++k)
                m_chunk_refs[fill[chunk_of(m_instances[t][k])]++] = {t, k};
    }
    m_lod.clear();
    for (const auto& list : m_instances)
        m_lod.emplace_back(list.size(), static_cast<uint8_t>(m_lod_max.size() - 1));
    m_near.assign(m_instances.size(),
                  std::vector<std::vector<world::BuildingInstance>>(m_lod_max.size()));
    m_near_doors = m_near;
    m_near_interior.assign(m_instances.size(), {});

    // Light column, authored directly in map space: X sweeps the full circumference
    // (so the projection wraps it into a tube), Z puts it k_column_radius off the axis.
    std::vector<world::Vertex>   cv;
    std::vector<uint32_t>        ci;
    const float z = static_cast<float>(m_radius - k_column_radius);
    for (int k = 0; k <= k_column_segments; ++k) {
        const float x = static_cast<float>(m_map_w * k / k_column_segments);
        for (const float y : {0.0f, static_cast<float>(m_map_h)}) {
            cv.push_back({{x, y, z}, {0.0f, 0.0f, -1.0f},
                          {k_column_rgb[0], k_column_rgb[1], k_column_rgb[2], 255}});
        }
        if (k > 0) {
            const uint32_t b = static_cast<uint32_t>(2 * (k - 1));
            ci.insert(ci.end(), {b, b + 2, b + 1, b + 1, b + 2, b + 3});
        }
    }
    m_column_vb = bgfx::createVertexBuffer(copy_vector(cv), layout);
    m_column_ib = bgfx::createIndexBuffer(copy_vector(ci), BGFX_BUFFER_INDEX32);
    return true;
}

void WorldRenderer::destroy()
{
    for (Group& g : m_groups) {
        destroy_handle(g.vb);
        destroy_handle(g.ib);
    }
    m_groups.clear();
    for (BuildingType& t : m_types) {
        for (auto& lod : t.lods) {
            for (Mesh& mesh : lod) {
                destroy_handle(mesh.vb);
                destroy_handle(mesh.ib);
            }
        }
        destroy_handle(t.instances);
    }
    m_types.clear();
    m_instances.clear();
    m_chunk_refs.clear();
    m_chunk_start.clear();
    m_lod.clear();
    m_near.clear();
    m_near_doors.clear();
    m_near_interior.clear();
    destroy_handle(m_column_vb);
    destroy_handle(m_column_ib);
    destroy_handle(m_terrain_prog);
    destroy_handle(m_building_prog);
    destroy_handle(u_cylinder);
    destroy_handle(u_offset);
    destroy_handle(u_cam_chunk);
    destroy_handle(u_grid);
    destroy_handle(u_material);
    destroy_handle(u_fog);
    destroy_handle(u_fog_color);
    destroy_handle(u_shadow_uv);
    destroy_handle(u_cloud_shadow);
    destroy_handle(s_cloud_shadow);
    destroy_handle(u_ring);
    destroy_handle(u_facade);
    destroy_handle(u_facade_color);
    destroy_handle(m_no_shadow);
}

void WorldRenderer::submit_buildings(bgfx::ViewId view, const Mesh& mesh, const bgfx::InstanceDataBuffer* near,
                                     bgfx::VertexBufferHandle far, uint32_t far_count, float mode,
                                     uint64_t state) const
{
    // Far draws skip the near ring's chunks; near draws carry only those chunks.
    const float ring[4]     = {static_cast<float>(m_ring), near ? 0.0f : 1.0f, 0, 0};
    const float material[4] = {0, mode, 0, 0};
    bgfx::setUniform(u_ring, ring);
    bgfx::setUniform(u_material, material);
    bgfx::setUniform(u_facade, m_facade.data());
    bgfx::setUniform(u_facade_color, m_facade_color.data());
    bgfx::setVertexBuffer(0, mesh.vb);
    bgfx::setIndexBuffer(mesh.ib);
    if (near) bgfx::setInstanceDataBuffer(near);
    else      bgfx::setInstanceDataBuffer(far, 0, far_count);
    bgfx::setState(state);
    bgfx::submit(view, m_building_prog);
}

void WorldRenderer::submit(bgfx::ViewId view, const CameraPos& cam_in, bool reversed_z)
{
    CameraPos cam = cam_in;
    cam.x = world::wrap_x(cam.x);

    const uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z
                         | BGFX_STATE_MSAA
                         | (reversed_z ? BGFX_STATE_DEPTH_TEST_GREATER : BGFX_STATE_DEPTH_TEST_LESS);

    const float cylinder[4] = {static_cast<float>(m_radius), static_cast<float>(cam.alt), 0, 0};
    const float fog_u[4]    = {fog.density, fog.opaque, fog.clear, 0};
    const float fog_color[4] = {((k_fog_rgba >> 24) & 0xff) / 255.0f, ((k_fog_rgba >> 16) & 0xff) / 255.0f,
                                ((k_fog_rgba >> 8) & 0xff) / 255.0f, 1.0f};
    // Shadow uv at the camera, taken modulo 1 in double so the per-vertex offset
    // added in the shader stays small.
    const bool  has_shadow = bgfx::isValid(cloud_shadow.texture) && cloud_shadow.strength > 0;
    const bgfx::TextureHandle shadow_tex = has_shadow ? cloud_shadow.texture : m_no_shadow;
    const double su = (cam.x - cloud_shadow.offset_x) / m_map_w;
    const double sv = (cam.y - cloud_shadow.offset_y) / m_map_h;
    const float shadow_uv[4] = {static_cast<float>(su - std::floor(su)), static_cast<float>(sv - std::floor(sv)),
                                static_cast<float>(1.0 / m_map_w), static_cast<float>(1.0 / m_map_h)};
    const float shadow_k[4]  = {has_shadow ? cloud_shadow.strength : 0.0f, 0, 0, 0};
    auto bind_shadow = [&] {
        bgfx::setUniform(u_shadow_uv, shadow_uv);
        bgfx::setUniform(u_cloud_shadow, shadow_k);
        bgfx::setTexture(0, s_cloud_shadow, shadow_tex);
    };

    const float lit[4]      = {0, 0, 0, 0};
    const float emissive[4] = {1, 0, 0, 0};

    // -- Terrain groups ------------------------------------------------------
    for (const Group& g : m_groups) {
        const float offset[4] = {static_cast<float>(world::wrap_dx(g.centre_x - cam.x)),
                                 static_cast<float>(g.centre_y - cam.y), 0, 0};
        bgfx::setUniform(u_cylinder, cylinder);
        bgfx::setUniform(u_fog, fog_u);
        bgfx::setUniform(u_fog_color, fog_color);
        bind_shadow();
        bgfx::setUniform(u_material, lit);
        bgfx::setUniform(u_offset, offset);
        bgfx::setVertexBuffer(0, g.vb);
        bgfx::setIndexBuffer(g.ib);
        bgfx::setState(state | BGFX_STATE_CULL_CW);
        bgfx::submit(view, m_terrain_prog);
    }

    // -- Light column (seen from below, from inside the tube's sweep: no culling)
    {
        const float offset[4] = {static_cast<float>(world::wrap_dx(-cam.x)),
                                 static_cast<float>(-cam.y), 0, 0};
        bgfx::setUniform(u_cylinder, cylinder);
        bgfx::setUniform(u_fog, fog_u);
        bgfx::setUniform(u_fog_color, fog_color);
        bind_shadow();
        bgfx::setUniform(u_material, emissive);
        bgfx::setUniform(u_offset, offset);
        bgfx::setVertexBuffer(0, m_column_vb);
        bgfx::setIndexBuffer(m_column_ib);
        bgfx::setState(state);
        bgfx::submit(view, m_terrain_prog);
    }

    // -- Buildings: one instanced draw per type for the whole cylinder ---------
    const double col = std::floor(cam.x / m_chunk_x);
    const double row = std::floor(cam.y / m_chunk_y);
    const float cam_chunk[4] = {static_cast<float>(col), static_cast<float>(row),
                                static_cast<float>(cam.x - (col + 0.5) * m_chunk_x),
                                static_cast<float>(cam.y - (row + 0.5) * m_chunk_y)};
    const float grid[4] = {static_cast<float>(m_chunk_x), static_cast<float>(m_chunk_y),
                           static_cast<float>(m_columns), 0};

    // Uniforms every building draw shares; bgfx clears them after each submit.
    auto bind_common = [&] {
        bgfx::setUniform(u_cylinder, cylinder);
        bgfx::setUniform(u_fog, fog_u);
        bgfx::setUniform(u_fog_color, fog_color);
        bind_shadow();
        bgfx::setUniform(u_cam_chunk, cam_chunk);
        bgfx::setUniform(u_grid, grid);
    };
    const uint64_t culled   = state | BGFX_STATE_CULL_CW;
    const size_t   lod_last = m_lod_max.size() - 1;
    m_stats.building_draws     = 0;
    m_stats.near_instances     = 0;
    m_stats.building_triangles = 0;

    // Far: the whole cylinder at the last LOD, minus the near ring.
    for (const BuildingType& t : m_types) {
        if (!t.count) continue;
        bind_common();
        submit_buildings(view, t.lods[lod_last][world::k_part_exterior], nullptr, t.instances, t.count,
                         m_lod_openings[lod_last] ? k_mode_masonry : k_mode_flat, culled);
        ++m_stats.building_draws;
    }

    // Near: per-instance LOD for every chunk in the ring.
    for (auto& per_type : m_near)
        for (auto& bucket : per_type) bucket.clear();
    for (auto& per_type : m_near_doors)
        for (auto& bucket : per_type) bucket.clear();
    for (auto& bucket : m_near_interior) bucket.clear();

    float lo2[8], hi2[8];   // squared thresholds to step finer / coarser, per band edge
    const uint8_t edges = static_cast<uint8_t>(std::min<size_t>(lod_last, 8));
    for (uint8_t e = 0; e < edges; ++e) {
        lo2[e] = m_lod_max[e] * (1 - k_lod_hysteresis);
        hi2[e] = m_lod_max[e] * (1 + k_lod_hysteresis);
        lo2[e] *= lo2[e];
        hi2[e] *= hi2[e];
    }
    const float interior2 = m_interior_max * m_interior_max;

    for (int dr = -m_ring; dr <= m_ring; ++dr) {
        const int r = static_cast<int>(row) + dr;
        if (r < 0 || r >= m_rows) continue;
        for (int dc = -m_ring; dc <= m_ring; ++dc) {
            const int    c     = ((static_cast<int>(col) + dc) % m_columns + m_columns) % m_columns;
            const size_t chunk = static_cast<size_t>(r) * m_columns + static_cast<size_t>(c);
            // Camera to chunk centre, in the same whole-chunk steps the shader uses.
            const double ox = dc * m_chunk_x - cam_chunk[2];
            const double oy = dr * m_chunk_y - cam_chunk[3];
            for (uint32_t k = m_chunk_start[chunk]; k < m_chunk_start[chunk + 1]; ++k) {
                const NearRef                 ref = m_chunk_refs[k];
                const world::BuildingInstance& b  = m_instances[ref.type][ref.index];
                const float dx = static_cast<float>(ox + b.x);
                const float dy = static_cast<float>(oy + b.y);
                const float dz = static_cast<float>(b.z - cam.alt);
                const float d2 = dx * dx + dy * dy + dz * dz;

                uint8_t& lod = m_lod[ref.type][ref.index];
                while (lod > 0 && d2 < lo2[lod - 1]) --lod;
                while (lod < edges && d2 > hi2[lod]) ++lod;
                m_near[ref.type][lod].push_back(b);
                // Door panes would seal the doorway the player walks through; where the
                // real interior is drawn there is something to see instead.
                if (d2 < interior2) m_near_interior[ref.type].push_back(b);
                else                m_near_doors[ref.type][lod].push_back(b);
            }
        }
    }

    auto upload = [](const std::vector<world::BuildingInstance>& list, bgfx::InstanceDataBuffer& idb) {
        const uint32_t n      = static_cast<uint32_t>(list.size());
        const uint16_t stride = sizeof(world::BuildingInstance);
        if (n == 0 || bgfx::getAvailInstanceDataBuffer(n, stride) < n) return false;
        bgfx::allocInstanceDataBuffer(&idb, n, stride);
        std::memcpy(idb.data, list.data(), list.size() * stride);
        return true;
    };

    for (size_t t = 0; t < m_types.size(); ++t) {
        const BuildingType& bt = m_types[t];
        for (size_t l = 0; l < m_near[t].size(); ++l) {
            const auto& list = m_near[t][l];
            bgfx::InstanceDataBuffer idb;
            if (!upload(list, idb)) continue;
            const auto& lod = bt.lods[l];
            const float mode = m_lod_openings[l] ? k_mode_masonry
                             : (l > 0 && m_lod_openings[l - 1]) ? k_mode_parallax : k_mode_flat;
            bind_common();
            submit_buildings(view, lod[world::k_part_exterior], &idb, BGFX_INVALID_HANDLE, 0, mode,
                             culled);
            uint64_t tris = lod[world::k_part_exterior].triangles;
            ++m_stats.building_draws;
            // Panes face out, so culling keeps them out of the way from inside.
            if (m_lod_openings[l] && bgfx::isValid(lod[world::k_part_glazing].vb)) {
                bind_common();
                submit_buildings(view, lod[world::k_part_glazing], &idb, BGFX_INVALID_HANDLE, 0,
                                 k_mode_glazing, culled);
                tris += lod[world::k_part_glazing].triangles;
                ++m_stats.building_draws;
            }
            m_stats.near_instances     += static_cast<uint32_t>(list.size());
            m_stats.building_triangles += tris * list.size();

            const Mesh&                 doors = lod[world::k_part_door_glazing];
            const auto&                 shut  = m_near_doors[t][l];
            bgfx::InstanceDataBuffer    didb;
            if (m_lod_openings[l] && bgfx::isValid(doors.vb) && upload(shut, didb)) {
                bind_common();
                submit_buildings(view, doors, &didb, BGFX_INVALID_HANDLE, 0, k_mode_glazing, culled);
                m_stats.building_triangles += uint64_t{doors.triangles} * shut.size();
                ++m_stats.building_draws;
            }
        }

        // Interiors: you are inside the surface, so draw both sides.
        const Mesh& interior = bt.lods[0][world::k_part_interior];
        bgfx::InstanceDataBuffer idb;
        if (bgfx::isValid(interior.vb) && upload(m_near_interior[t], idb)) {
            bind_common();
            submit_buildings(view, interior, &idb, BGFX_INVALID_HANDLE, 0, k_mode_interior, state);
            m_stats.building_triangles += uint64_t{interior.triangles} * m_near_interior[t].size();
            ++m_stats.building_draws;
        }
    }
}

} // namespace render
