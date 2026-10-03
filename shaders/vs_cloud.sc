$input a_position, i_data0, i_data1
$output v_texcoord0, v_cloud, v_upView, v_relPos, v_cloudAux

#include <bgfx_shader.sh>
#include "cylinder.sh"

// One camera-facing quad per cloud puff, rebuilt on the CPU every frame (sorted
// back to front, camera-relative, so no large coordinates reach the GPU).
// i_data0: xyz puff centre in map space relative to the camera (x wrapped; z is
//          absolute altitude, as cyl_position expects), w puff radius
// i_data1: x cloud base altitude, y cloud height, z cloud shade seed, w opacity
//
// The fragment shader ray-traces each puff as a sphere sliced flat at the cloud
// base, so it needs the ray (the fragment's view-space position; the eye is the
// origin) and the sphere in view space.
void main()
{
    vec3 tangent, up;
    vec3 c = cyl_position(i_data0.xyz, tangent, up);

    // Expand in view space so the quad faces the camera. A sphere's silhouette is a
    // little wider than its radius when seen up close, hence the margin.
    vec4 centre = mul(u_view, vec4(c, 1.0));
    vec4 vc     = centre;
    vc.xy += a_position.xy * i_data0.w * 1.2;

    v_texcoord0 = a_position.xy;
    v_upView    = mul(u_view, vec4(up, 0.0)).xyz;
    v_cloud     = vec4(i_data1.x - i_data0.z, i_data0.w, i_data1.y, i_data1.z);
    v_cloudAux  = vec4(centre.xyz, i_data1.w);
    v_relPos    = vc.xyz;
    gl_Position = mul(u_proj, vc);
}
