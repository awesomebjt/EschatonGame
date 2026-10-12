#pragma once

#include <cstdint>
#include <vector>

#include <bgfx/bgfx.h>

namespace render {

struct CameraPos;
struct FogParams;

// One player to draw: map position of the feet, facing, colour.
struct Avatar
{
    double x = 0, y = 0, alt = 0;
    float  yaw = 0;
    float  rgb[3] = {1, 1, 1};
};

// Other players as their collision capsule (radius and height from the
// player's CapsuleCollider), one instanced draw for all of them.
class AvatarRenderer
{
public:
    bool create(double radius, float capsule_radius, float capsule_height);
    void destroy();

    void submit(bgfx::ViewId view, const CameraPos& cam, bool reversed_z, const FogParams& fog,
                const std::vector<Avatar>& avatars);

private:
    double                   m_radius = 0;
    bgfx::VertexBufferHandle m_vb   = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle  m_ib   = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle      m_prog = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle      u_cylinder  = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle      u_fog       = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle      u_fog_color = BGFX_INVALID_HANDLE;
};

} // namespace render
