$input a_position, a_normal, a_color0
$output v_color0, v_normal, v_up, v_relPos

#include <bgfx_shader.sh>
#include "cylinder.sh"

uniform vec4 u_offset;      // xy: mesh origin minus camera, map space, x wrapped

void main()
{
    vec3 tangent, up;
    vec3 p = cyl_position(vec3(a_position.xy + u_offset.xy, a_position.z), tangent, up);

    v_color0 = a_color0;
    v_normal = cyl_direction(a_normal, tangent, up);
    v_up     = up;
    v_relPos = p;
    gl_Position = mul(u_viewProj, vec4(p, 1.0));
}
