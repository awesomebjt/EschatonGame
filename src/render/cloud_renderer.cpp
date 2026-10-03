#include "render/cloud_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "render/shader.h"
#include "render/world_renderer.h"
#include "world/wrap.h"

namespace render {

namespace {

// ~49 m per texel over the 25.1 × 32 km floor: clouds are 300–900 m across and the
// shadows are soft along the axis anyway.
constexpr int k_shadow_w = 512;

struct PuffInstance
{
    float x, y, alt, radius;
    float base, height, shade, opacity;
};
static_assert(sizeof(PuffInstance) == 32);

} // namespace

bool CloudRenderer::create(double radius, double map_w, double map_h)
{
    m_radius = radius;
    m_map_w  = map_w;
    m_map_h  = map_h;

    m_prog = load_program("vs_cloud.sc", "fs_cloud.sc");
    if (!bgfx::isValid(m_prog)) return false;

    bgfx::VertexLayout layout;
    layout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).end();
    const float quad[6][3] = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
    m_quad = bgfx::createVertexBuffer(bgfx::copy(quad, sizeof(quad)), layout);

    // Shared by name with the world renderer; bgfx refcounts them.
    u_cylinder  = bgfx::createUniform("u_cylinder", bgfx::UniformType::Vec4);
    u_fog       = bgfx::createUniform("u_fog", bgfx::UniformType::Vec4);
    u_fog_color = bgfx::createUniform("u_fogColor", bgfx::UniformType::Vec4);

    set_params(m_params);
    return true;
}

void CloudRenderer::destroy()
{
    if (bgfx::isValid(m_prog))   bgfx::destroy(m_prog);
    if (bgfx::isValid(m_quad))   bgfx::destroy(m_quad);
    if (bgfx::isValid(m_shadow)) bgfx::destroy(m_shadow);
    for (auto* u : {&u_cylinder, &u_fog, &u_fog_color}) if (bgfx::isValid(*u)) bgfx::destroy(*u);
    m_prog = BGFX_INVALID_HANDLE;
    m_quad = BGFX_INVALID_HANDLE;
    m_shadow = BGFX_INVALID_HANDLE;
    u_cylinder = u_fog = u_fog_color = BGFX_INVALID_HANDLE;
    m_field = {};
}

void CloudRenderer::set_params(const world::CloudParams& params)
{
    m_params = params;
    m_field  = world::generate_clouds(params, m_map_w, m_map_h, m_radius);

    const int sh = static_cast<int>(std::lround(k_shadow_w * m_map_h / m_map_w));
    const std::vector<uint8_t> shadow =
        world::cloud_shadow_map(m_field, params, k_shadow_w, sh, m_map_w, m_map_h, m_radius);
    if (bgfx::isValid(m_shadow)) bgfx::destroy(m_shadow);
    // Default sampler state: linear, repeat in both axes, matching the field's wrap.
    m_shadow = bgfx::createTexture2D(static_cast<uint16_t>(k_shadow_w), static_cast<uint16_t>(sh), false, 1,
                                     bgfx::TextureFormat::R8, 0, bgfx::copy(shadow.data(), uint32_t(shadow.size())));
}

void CloudRenderer::update(double dt)
{
    // Wind is true metres per second at the cloud layer; map x is floor metres.
    const double sx = world::cloud_x_scale(m_radius, m_params.base_alt);
    m_offset_x = std::fmod(m_offset_x + m_params.wind_x * sx * dt, m_map_w);
    m_offset_y = std::fmod(m_offset_y + m_params.wind_y * dt, m_map_h);
}

void CloudRenderer::submit(bgfx::ViewId view, const CameraPos& cam, bool reversed_z, const FogParams& fog)
{
    m_drawn = 0;
    if (!enabled || !bgfx::isValid(m_prog) || m_field.clouds.empty()) return;

    // Gather visible puffs with their camera distance, using the same projection as
    // cyl_position. Anything past the opaque fog distance is invisible anyway.
    const double R = m_radius;
    m_sorted.clear();
    for (uint32_t ci = 0; ci < m_field.clouds.size(); ++ci) {
        const world::Cloud& c = m_field.clouds[ci];
        const double cdx = world::wrap_dx(c.x + m_offset_x - cam.x);
        double       cdy = c.y + m_offset_y - cam.y;
        cdy -= m_map_h * std::round(cdy / m_map_h);
        // Coarse reject: a whole cloud far beyond the fog in map distance.
        if (std::fabs(cdy) > fog.opaque + 1000.0) continue;

        for (uint32_t k = 0; k < c.puff_count; ++k) {
            const uint32_t   pi  = c.first_puff + k;
            const world::Puff& p = m_field.puffs[pi];
            const double alt = c.base + p.z;
            const double dx  = cdx + p.x * world::cloud_x_scale(R, alt);
            const double dy  = cdy + p.y;
            const double th  = dx / R, r = R - alt, s = std::sin(0.5 * th);
            const double rx  = r * std::sin(th);
            const double ry  = alt - cam.alt + 2.0 * r * s * s;
            const double d   = std::sqrt(rx * rx + ry * ry + dy * dy);
            if (d - p.radius > fog.opaque) continue;
            m_sorted.push_back({static_cast<float>(d), ci, pi,
                                {static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(alt)}});
        }
    }
    if (m_sorted.empty()) return;
    std::sort(m_sorted.begin(), m_sorted.end(), [](const Sorted& a, const Sorted& b) { return a.dist > b.dist; });

    const uint32_t n = bgfx::getAvailInstanceDataBuffer(static_cast<uint32_t>(m_sorted.size()), sizeof(PuffInstance));
    if (n == 0) return;
    bgfx::InstanceDataBuffer idb;
    bgfx::allocInstanceDataBuffer(&idb, n, sizeof(PuffInstance));
    auto* out = reinterpret_cast<PuffInstance*>(idb.data);
    // If the buffer came up short, keep the nearest puffs (the end of the sort).
    const size_t first = m_sorted.size() - n;
    for (uint32_t k = 0; k < n; ++k) {
        const Sorted&       s = m_sorted[first + k];
        const world::Cloud& c = m_field.clouds[s.cloud];
        const float         r = m_field.puffs[s.puff].radius;
        // Fade puffs the camera is in or near, so noclip can fly through cloud.
        const float opacity = std::clamp((s.dist - 0.5f * r) / r, 0.0f, 1.0f);
        out[k] = {s.pos[0], s.pos[1], s.pos[2], r, c.base, c.height, c.shade, opacity};
    }
    m_drawn = n;

    const float cylinder[4]  = {static_cast<float>(R), static_cast<float>(cam.alt), 0, 0};
    const float fog_u[4]     = {fog.density, fog.opaque, fog.clear, 0};
    const float fog_color[4] = {((k_fog_rgba >> 24) & 0xff) / 255.0f, ((k_fog_rgba >> 16) & 0xff) / 255.0f,
                                ((k_fog_rgba >> 8) & 0xff) / 255.0f, 1.0f};
    bgfx::setUniform(u_cylinder, cylinder);
    bgfx::setUniform(u_fog, fog_u);
    bgfx::setUniform(u_fog_color, fog_color);
    bgfx::setVertexBuffer(0, m_quad);
    bgfx::setInstanceDataBuffer(&idb);
    // Depth-tested against the city (towers can hide low clouds near the horizon),
    // never written: the puffs overlap and are blended in sorted order.
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA
                   | (reversed_z ? BGFX_STATE_DEPTH_TEST_GREATER : BGFX_STATE_DEPTH_TEST_LESS));
    bgfx::submit(view, m_prog);
}

} // namespace render
