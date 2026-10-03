$input a_position, a_normal, a_color0, i_data0, i_data1
$output v_color0, v_normal, v_up, v_relPos, v_shadowUv

#include <bgfx_shader.sh>
#include "cylinder.sh"

// i_data0: x, y, z, rot   position relative to the owning chunk's centre
// i_data1: col, row, shade
uniform vec4 u_camChunk;    // xy: camera's chunk column and row, zw: camera minus that chunk's centre
uniform vec4 u_grid;        // xy: chunk size in metres, z: column count
uniform vec4 u_shadowUv;    // xy: camera's cloud-shadow uv, zw: uv per map metre

void main()
{
    // Rebuild the camera-relative origin from small exact pieces: whole-chunk
    // steps (exact in float) plus two sub-chunk offsets.
    float dcol = i_data1.x - u_camChunk.x;
    dcol -= u_grid.z * floor(dcol / u_grid.z + 0.5);
    vec2 origin = vec2(dcol, i_data1.y - u_camChunk.y) * u_grid.xy + i_data0.xy - u_camChunk.zw;

    vec3 tangent, up;
    vec3 base = cyl_position(vec3(origin, i_data0.z), tangent, up);

    // Buildings are rigid: the prototype stands in the tangent plane at its origin.
    float cr = cos(i_data0.w);
    float sr = sin(i_data0.w);
    vec3 q = vec3(cr * a_position.x - sr * a_position.y,
                  sr * a_position.x + cr * a_position.y,
                  a_position.z);
    // The floor curves away under a rigid footprint (x² / 2R, 0.3 m at 50 m), so
    // sink the base ring to meet it rather than leave the corners floating.
    if (a_position.z < 0.01)
        q.z -= q.x * q.x / (2.0 * u_cylinder.x) + 0.1;

    vec3 n = vec3(cr * a_normal.x - sr * a_normal.y,
                  sr * a_normal.x + cr * a_normal.y,
                  a_normal.z);

    vec3 p = base + cyl_direction(q, tangent, up);

    // Small per-building brightness variation so identical rows don't read as one slab.
    v_color0 = vec4(a_color0.rgb * (0.86 + 0.24 * i_data1.z), a_color0.a);
    v_normal = cyl_direction(n, tangent, up);
    v_up     = up;
    v_relPos = p;
    // Rigid footprint: shadow by the vertex's map position, ignoring the tiny tilt.
    v_shadowUv = u_shadowUv.xy + (origin + q.xy) * u_shadowUv.zw;
    gl_Position = mul(u_viewProj, vec4(p, 1.0));
}
