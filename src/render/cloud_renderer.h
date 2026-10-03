#pragma once

#include <cstdint>
#include <vector>

#include <bgfx/bgfx.h>

#include "world/clouds.h"

namespace render {

struct CameraPos;
struct FogParams;

// Scattered cumulus drawn as soft sphere-impostor puffs (one instanced draw,
// sorted back to front each frame), plus the cloud shadow map the world shaders
// sample. The whole field drifts with the wind as one; the shadow map is built
// once per field and offset rather than redrawn.
class CloudRenderer
{
public:
    bool create(double radius, double map_w, double map_h);
    void destroy();

    // Regenerates the field and its shadow map (~0.7 s, mostly the axial blur).
    void set_params(const world::CloudParams& params);
    const world::CloudParams& params() const { return m_params; }

    // Wind doesn't change the field's shape, so it applies without regenerating.
    void set_wind(float spinward, float axial)
    {
        m_params.wind_x = spinward;
        m_params.wind_y = axial;
    }

    void update(double dt);   // wind drift
    void submit(bgfx::ViewId view, const CameraPos& cam, bool reversed_z, const FogParams& fog);

    bgfx::TextureHandle shadow_texture() const { return m_shadow; }
    double offset_x() const { return m_offset_x; }
    double offset_y() const { return m_offset_y; }

    size_t cloud_count() const { return m_field.clouds.size(); }
    size_t puff_count() const { return m_field.puffs.size(); }
    uint32_t drawn_puffs() const { return m_drawn; }

    bool enabled = true;

private:
    struct Sorted
    {
        float    dist;
        uint32_t cloud, puff;
        float    pos[3];   // camera-relative map position (x, y) and altitude
    };

    double m_radius = 0, m_map_w = 0, m_map_h = 0;
    double m_offset_x = 0, m_offset_y = 0;   // wind drift, map metres, wrapped

    world::CloudParams  m_params;
    world::CloudField   m_field;
    std::vector<Sorted> m_sorted;
    uint32_t            m_drawn = 0;

    bgfx::ProgramHandle      m_prog   = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle m_quad   = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle      m_shadow = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle      u_cylinder  = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle      u_fog       = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle      u_fog_color = BGFX_INVALID_HANDLE;
};

} // namespace render
