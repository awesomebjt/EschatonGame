#include "render/world_renderer.h"

#include <cmath>
#include <cstdio>

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
    m_stats   = {};

    m_terrain_prog  = load_program("vs_terrain.sc", "fs_world.sc");
    m_building_prog = load_program("vs_building.sc", "fs_world.sc");
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

    // Instancing needs no caps check: bgfx's renderer baseline guarantees it on
    // every backend (BGFX_CAPS_INSTANCING was removed upstream for that reason).
    {
        const bgfx::VertexLayout inst_layout = instance_layout();
        for (size_t t = 0; t < w.buildings.size(); ++t) {
            BuildingType bt;
            bt.count = static_cast<uint32_t>(w.buildings[t].size());
            if (bt.count) {
                const world::PrototypeMesh& p = w.prototypes[t];
                bt.mesh_vb   = bgfx::createVertexBuffer(copy_vector(p.vertices), layout);
                bt.mesh_ib   = bgfx::createIndexBuffer(copy_vector(p.indices), BGFX_BUFFER_INDEX32);
                bt.instances = bgfx::createVertexBuffer(copy_vector(w.buildings[t]), inst_layout);
                ++m_stats.building_draws;
                m_stats.building_instances += bt.count;
            }
            m_types.push_back(bt);
        }
    }

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
        destroy_handle(t.mesh_vb);
        destroy_handle(t.mesh_ib);
        destroy_handle(t.instances);
    }
    m_types.clear();
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
}

void WorldRenderer::submit(bgfx::ViewId view, const CameraPos& cam_in, bool reversed_z) const
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
    const float lit[4]      = {0, 0, 0, 0};
    const float emissive[4] = {1, 0, 0, 0};

    // -- Terrain groups ------------------------------------------------------
    for (const Group& g : m_groups) {
        const float offset[4] = {static_cast<float>(world::wrap_dx(g.centre_x - cam.x)),
                                 static_cast<float>(g.centre_y - cam.y), 0, 0};
        bgfx::setUniform(u_cylinder, cylinder);
        bgfx::setUniform(u_fog, fog_u);
        bgfx::setUniform(u_fog_color, fog_color);
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

    for (const BuildingType& t : m_types) {
        if (!t.count) continue;
        bgfx::setUniform(u_cylinder, cylinder);
        bgfx::setUniform(u_fog, fog_u);
        bgfx::setUniform(u_fog_color, fog_color);
        bgfx::setUniform(u_material, lit);
        bgfx::setUniform(u_cam_chunk, cam_chunk);
        bgfx::setUniform(u_grid, grid);
        bgfx::setVertexBuffer(0, t.mesh_vb);
        bgfx::setIndexBuffer(t.mesh_ib);
        bgfx::setInstanceDataBuffer(t.instances, 0, t.count);
        bgfx::setState(state | BGFX_STATE_CULL_CW);
        bgfx::submit(view, m_building_prog);
    }
}

} // namespace render
