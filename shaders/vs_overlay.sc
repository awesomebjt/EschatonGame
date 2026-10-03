$input a_position

#include <bgfx_shader.sh>

// Screen-space quads for 2D overlays (the debug console backdrop); positions are
// pixels under an orthographic view projection.
void main()
{
    gl_Position = mul(u_viewProj, vec4(a_position, 1.0));
}
