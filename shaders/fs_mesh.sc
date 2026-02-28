$input v_texcoord0, v_viewPos

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);

void main()
{
    vec4 texColor = texture2D(s_texColor, v_texcoord0);

    // Sky-blue haze: fully clear below 3500 units, fully opaque at 7000 units.
    float dist = length(v_viewPos);
    vec3 fogColor = vec3(0.53, 0.81, 0.92);
    float fogFactor = clamp((dist - 3500.0) / 3500.0, 0.0, 1.0);

    gl_FragColor = vec4(mix(texColor.rgb, fogColor, fogFactor), texColor.a);
}
