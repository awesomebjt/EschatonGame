#pragma once

#include <cstdint>
#include <vector>

#include <bgfx/bgfx.h>

namespace world { struct WorldData; }

namespace render {

// Camera position in map space (x wraps at the circumference, alt is metres off
// the wall). Orientation lives in the view matrix; the world renderer only needs
// position because it builds every vertex relative to the camera.
struct CameraPos
{
    double x = 0, y = 0, alt = 0;
};

// Haze: none inside `clear` metres, then exponential at `density` per metre,
// rescaled to reach full opacity at `opaque`. The far side (~8 km overhead) is
// lost in it entirely until the backdrop shell exists.
struct FogParams
{
    float density = 3.0e-4f;
    float opaque  = 7000.0f;
    float clear   = 1500.0f;
};

// Cloud shadow map over the whole floor (u = x / map_w, v = y / map_h, both
// repeating), sampled by map position. The cloud field drifts as one, so the map
// never changes as it moves; the drift is an offset into it.
struct CloudShadowBinding
{
    bgfx::TextureHandle texture  = BGFX_INVALID_HANDLE;   // R8; none = no shadow
    float               strength = 1.0f;
    double              offset_x = 0, offset_y = 0;       // field drift, map metres
};

struct WorldStats
{
    uint32_t terrain_draws = 0;
    uint32_t building_draws = 0;
    uint64_t terrain_triangles = 0;
    uint64_t building_instances = 0;
};

// Owns the GPU copy of a loaded cylinder: merged terrain groups, one instance
// buffer per building type, and the central light column.
class WorldRenderer
{
public:
    bool create(const world::WorldData& world);
    void destroy();

    // Submits the whole cylinder to `view`. The caller sets the view transform
    // with the eye at the origin; `reversed_z` selects the depth test.
    void submit(bgfx::ViewId view, const CameraPos& cam, bool reversed_z) const;

    const WorldStats& stats() const { return m_stats; }

    // Read every submit, so they can be tuned live (the debug console does).
    FogParams          fog;
    CloudShadowBinding cloud_shadow;

private:
    struct Group
    {
        double                  centre_x = 0, centre_y = 0;
        bgfx::VertexBufferHandle vb = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle  ib = BGFX_INVALID_HANDLE;
    };
    struct BuildingType
    {
        bgfx::VertexBufferHandle mesh_vb  = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle  mesh_ib  = BGFX_INVALID_HANDLE;
        bgfx::VertexBufferHandle instances = BGFX_INVALID_HANDLE;
        uint32_t                 count = 0;
    };

    std::vector<Group>        m_groups;
    std::vector<BuildingType> m_types;
    bgfx::VertexBufferHandle  m_column_vb = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle   m_column_ib = BGFX_INVALID_HANDLE;

    bgfx::ProgramHandle m_terrain_prog  = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_building_prog = BGFX_INVALID_HANDLE;

    bgfx::UniformHandle u_cylinder  = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_offset    = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_cam_chunk = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_grid      = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_material  = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_fog       = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_fog_color = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_shadow_uv = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_cloud_shadow = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_cloud_shadow = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_no_shadow    = BGFX_INVALID_HANDLE;   // 1×1 zero

    double   m_radius = 0, m_map_w = 0, m_map_h = 0;
    double   m_chunk_x = 0, m_chunk_y = 0;
    int      m_columns = 0;
    WorldStats m_stats;
};

// Haze colour; also the clear colour, so unlit space reads as distant air.
inline constexpr uint32_t k_fog_rgba = 0xB4C3D2ff;

} // namespace render
