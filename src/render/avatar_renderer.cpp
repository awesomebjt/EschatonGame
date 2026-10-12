#include "render/avatar_renderer.h"

#include <cmath>
#include <numbers>

#include "render/shader.h"
#include "render/world_renderer.h"
#include "world/wrap.h"

namespace render {

namespace {

struct Vertex
{
    float pos[3];
    float nrm[3];
};

// Capsule standing on the origin along +z: a cylinder from r to h − r capped by
// two hemispheres, `segments` around and `rings` per hemisphere.
void build_capsule(float r, float h, int segments, int rings, std::vector<Vertex>& vertices,
                   std::vector<uint16_t>& indices)
{
    const float pi = std::numbers::pi_v<float>;
    // Latitude rows from the bottom pole to the top pole; the two equators are
    // separate rows, so the straight section is the quad strip between them.
    std::vector<std::pair<float, float>> rows;   // (z of the sphere centre offset, latitude)
    for (int k = 0; k <= rings; ++k) rows.push_back({r, -0.5f * pi + 0.5f * pi * k / rings});
    for (int k = 0; k <= rings; ++k) rows.push_back({h - r, 0.5f * pi * k / rings});

    for (const auto& [cz, lat] : rows) {
        for (int s = 0; s <= segments; ++s) {
            const float a  = 2.0f * pi * s / segments;
            const float nx = std::cos(lat) * std::cos(a);
            const float ny = std::cos(lat) * std::sin(a);
            const float nz = std::sin(lat);
            vertices.push_back({{r * nx, r * ny, cz + r * nz}, {nx, ny, nz}});
        }
    }
    const int stride = segments + 1;
    for (size_t row = 0; row + 1 < rows.size(); ++row) {
        for (int s = 0; s < segments; ++s) {
            const uint16_t a = static_cast<uint16_t>(row * stride + s);
            const uint16_t b = static_cast<uint16_t>(a + stride);
            // Counter-clockwise seen from outside.
            indices.insert(indices.end(), {a, static_cast<uint16_t>(a + 1), b,
                                           static_cast<uint16_t>(a + 1), static_cast<uint16_t>(b + 1), b});
        }
    }
}

} // namespace

bool AvatarRenderer::create(double radius, float capsule_radius, float capsule_height)
{
    destroy();
    m_radius = radius;
    m_prog   = load_program("vs_avatar.sc", "fs_avatar.sc");
    if (!bgfx::isValid(m_prog)) return false;

    std::vector<Vertex>   vertices;
    std::vector<uint16_t> indices;
    build_capsule(capsule_radius, capsule_height, 20, 6, vertices, indices);

    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
        .end();
    m_vb = bgfx::createVertexBuffer(bgfx::copy(vertices.data(), uint32_t(vertices.size() * sizeof(Vertex))), layout);
    m_ib = bgfx::createIndexBuffer(bgfx::copy(indices.data(), uint32_t(indices.size() * sizeof(uint16_t))));

    u_cylinder  = bgfx::createUniform("u_cylinder", bgfx::UniformType::Vec4);
    u_fog       = bgfx::createUniform("u_fog", bgfx::UniformType::Vec4);
    u_fog_color = bgfx::createUniform("u_fogColor", bgfx::UniformType::Vec4);
    return true;
}

void AvatarRenderer::destroy()
{
    if (bgfx::isValid(m_vb)) bgfx::destroy(m_vb);
    if (bgfx::isValid(m_ib)) bgfx::destroy(m_ib);
    if (bgfx::isValid(m_prog)) bgfx::destroy(m_prog);
    for (auto* u : {&u_cylinder, &u_fog, &u_fog_color}) if (bgfx::isValid(*u)) bgfx::destroy(*u);
    m_vb = BGFX_INVALID_HANDLE;
    m_ib = BGFX_INVALID_HANDLE;
    m_prog = BGFX_INVALID_HANDLE;
    u_cylinder = u_fog = u_fog_color = BGFX_INVALID_HANDLE;
}

void AvatarRenderer::submit(bgfx::ViewId view, const CameraPos& cam, bool reversed_z, const FogParams& fog,
                            const std::vector<Avatar>& avatars)
{
    if (!bgfx::isValid(m_prog) || avatars.empty()) return;
    const uint32_t n      = static_cast<uint32_t>(avatars.size());
    const uint16_t stride = 32;
    if (bgfx::getAvailInstanceDataBuffer(n, stride) < n) return;

    bgfx::InstanceDataBuffer idb;
    bgfx::allocInstanceDataBuffer(&idb, n, stride);
    float* out = reinterpret_cast<float*>(idb.data);
    for (const Avatar& a : avatars) {
        // Camera-relative in double first, so only small numbers reach the GPU.
        out[0] = static_cast<float>(world::wrap_dx(a.x - cam.x));
        out[1] = static_cast<float>(a.y - cam.y);
        out[2] = static_cast<float>(a.alt);
        out[3] = a.yaw;
        out[4] = a.rgb[0];
        out[5] = a.rgb[1];
        out[6] = a.rgb[2];
        out[7] = 0;
        out += 8;
    }

    const float cylinder[4] = {static_cast<float>(m_radius), static_cast<float>(cam.alt), 0, 0};
    const float fog_u[4]    = {fog.density, fog.opaque, fog.clear, 0};
    float fog_color[4];
    fog.color(fog_color);
    bgfx::setUniform(u_cylinder, cylinder);
    bgfx::setUniform(u_fog, fog_u);
    bgfx::setUniform(u_fog_color, fog_color);
    bgfx::setVertexBuffer(0, m_vb);
    bgfx::setIndexBuffer(m_ib);
    bgfx::setInstanceDataBuffer(&idb);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_MSAA
                   | BGFX_STATE_CULL_CW
                   | (reversed_z ? BGFX_STATE_DEPTH_TEST_GREATER : BGFX_STATE_DEPTH_TEST_LESS));
    bgfx::submit(view, m_prog);
}

} // namespace render
