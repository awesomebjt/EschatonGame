#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <bgfx/bgfx.h>

#include "world/world_loader.h"   // BuildingInstance, held by value for the near ring

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
    uint32_t building_draws = 0;          // last frame, far + near
    uint64_t terrain_triangles = 0;
    uint64_t building_instances = 0;
    uint32_t near_instances = 0;          // last frame, in the per-instance LOD ring
    uint64_t building_triangles = 0;      // last frame, near ring only (far is ~20 per building)
};

// Owns the GPU copy of a loaded cylinder: merged terrain groups, building
// prototypes at every LOD, and the central light column.
//
// Buildings draw in two parts. Far: one instanced draw per type for the whole
// cylinder at the coarsest LOD, from a static buffer; the vertex shader drops
// instances in the chunks around the camera. Near: those chunks' instances are
// re-bucketed every frame by (type, LOD) from their own distance, with hysteresis,
// and drawn from transient buffers, with glazing and interiors where the band
// has them.
class WorldRenderer
{
public:
    bool create(const world::WorldData& world);
    void destroy();

    // Submits the whole cylinder to `view`. The caller sets the view transform
    // with the eye at the origin; `reversed_z` selects the depth test.
    void submit(bgfx::ViewId view, const CameraPos& cam, bool reversed_z);

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
    struct Mesh
    {
        bgfx::VertexBufferHandle vb = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle  ib = BGFX_INVALID_HANDLE;
        uint32_t                 triangles = 0;
    };
    struct BuildingType
    {
        std::vector<std::array<Mesh, world::k_part_count>> lods;   // [lod][part]; no collision
        bgfx::VertexBufferHandle instances = BGFX_INVALID_HANDLE;   // all of them, for the far draw
        uint32_t                 count = 0;
    };
    struct NearRef
    {
        uint32_t type, index;   // into m_instances
    };

    void submit_buildings(bgfx::ViewId view, const Mesh& mesh, const bgfx::InstanceDataBuffer* near,
                          bgfx::VertexBufferHandle far, uint32_t far_count, float mode,
                          uint64_t state) const;

    std::vector<Group>        m_groups;
    std::vector<BuildingType> m_types;

    // CPU side of the near ring: every instance by type, each chunk's instances
    // (CSR over row * columns + col), and each instance's current LOD.
    std::vector<std::vector<world::BuildingInstance>> m_instances;
    std::vector<uint32_t>             m_chunk_start;
    std::vector<NearRef>              m_chunk_refs;
    std::vector<std::vector<uint8_t>> m_lod;
    std::vector<float>                m_lod_max;        // band ends, metres
    std::vector<bool>                 m_lod_openings;
    float                             m_interior_max = 0;
    int                               m_ring = 0;       // near ring radius, in chunks
    std::array<float, 4>              m_facade{}, m_facade_color{};
    // Per-frame scratch, kept to avoid reallocating: [type][lod], and [type]. Door
    // panes are bucketed apart: only instances whose interior isn't drawn get them.
    std::vector<std::vector<std::vector<world::BuildingInstance>>> m_near;
    std::vector<std::vector<std::vector<world::BuildingInstance>>> m_near_doors;
    std::vector<std::vector<world::BuildingInstance>>              m_near_interior;
    bgfx::VertexBufferHandle  m_column_vb = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle   m_column_ib = BGFX_INVALID_HANDLE;

    bgfx::ProgramHandle m_terrain_prog  = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_building_prog = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_ring          = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_facade        = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_facade_color  = BGFX_INVALID_HANDLE;

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
    int      m_columns = 0, m_rows = 0;
    WorldStats m_stats;
};

// Haze colour; also the clear colour, so unlit space reads as distant air.
inline constexpr uint32_t k_fog_rgba = 0xB4C3D2ff;

} // namespace render
