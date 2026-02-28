$input a_position, a_texcoord0
$output v_texcoord0, v_viewPos

#include <bgfx_shader.sh>

void main()
{
    vec4 worldPos = mul(u_model[0], vec4(a_position, 1.0));
    vec4 viewPos  = mul(u_view, worldPos);
    gl_Position   = mul(u_proj, viewPos);
    v_texcoord0   = a_texcoord0;
    v_viewPos     = viewPos.xyz;
}
