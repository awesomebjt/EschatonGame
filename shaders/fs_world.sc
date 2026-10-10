$input v_color0, v_normal, v_up, v_relPos, v_shadowUv

#include <bgfx_shader.sh>
#include "fog.sh"
#include "lighting.sh"

uniform vec4 u_material;      // x: 1 = emissive (the light column), 0 = lit
uniform vec4 u_cloudShadow;   // x: shadow strength (0 = off)
SAMPLER2D(s_cloudShadow, 0);  // fraction of the column hidden by cloud, over the map

void main()
{
    vec3 n  = normalize(v_normal);
    vec3 up = normalize(v_up);

    float shadow = min(texture2D(s_cloudShadow, v_shadowUv).r * u_cloudShadow.x, 1.0);
    float light  = column_light(n, up, shadow);
    vec3 col = v_color0.rgb * mix(light, 1.0, u_material.x);

    float fog = fog_amount(length(v_relPos)) * (1.0 - 0.7 * u_material.x);
    gl_FragColor = vec4(mix(col, u_fogColor.rgb, fog), 1.0);
}
