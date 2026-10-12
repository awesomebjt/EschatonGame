$input a_position, a_normal, i_data0, i_data1
$output v_color0, v_normal, v_up, v_relPos, v_tangent

#include <bgfx_shader.sh>
#include "cylinder.sh"

// Other players: a capsule mesh standing on its feet at the origin, local +y
// forward, instanced once per player.
// i_data0: xy map position relative to the camera (x wrapped), z altitude of the
//          feet (absolute, as cyl_position expects), w yaw (0 faces +y)
// i_data1: rgb player colour
void main()
{
    vec3 tangent, up;
    vec3 base = cyl_position(i_data0.xyz, tangent, up);

    // Yaw 0 looks along +y; positive yaw turns toward +x, like the camera.
    float s = sin(i_data0.w);
    float c = cos(i_data0.w);
    vec2  right = vec2(c, -s);
    vec2  fwd   = vec2(s, c);
    vec3  q = vec3(a_position.x * right + a_position.y * fwd, a_position.z);
    vec3  n = vec3(a_normal.x * right + a_normal.y * fwd, a_normal.z);

    vec3 p = base + cyl_direction(q, tangent, up);
    v_color0    = vec4(i_data1.rgb, 1.0);
    v_normal    = cyl_direction(n, tangent, up);
    v_up        = up;
    v_relPos    = p;
    v_tangent   = a_position;   // local position, for the visor
    gl_Position = mul(u_viewProj, vec4(p, 1.0));
}
