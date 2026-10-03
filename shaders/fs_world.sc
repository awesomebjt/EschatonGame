$input v_color0, v_normal, v_up, v_relPos, v_shadowUv

#include <bgfx_shader.sh>
#include "fog.sh"

uniform vec4 u_material;      // x: 1 = emissive (the light column), 0 = lit
uniform vec4 u_cloudShadow;   // x: shadow strength (0 = off)
SAMPLER2D(s_cloudShadow, 0);  // fraction of the column hidden by cloud, over the map

void main()
{
    vec3 n  = normalize(v_normal);
    vec3 up = normalize(v_up);

    // The central column is a line light along the axis. Faces toward the axis get
    // most of it, faces looking along the axis catch its oblique run, and faces
    // looking around the circumference see it edge-on and get mostly ambient. The
    // small tangent term only separates the two circumferential wall directions.
    // Cloud shadow dims the column's share, not the ambient.
    vec3  side   = cross(up, vec3(0.0, 0.0, 1.0));
    float shadow = min(texture2D(s_cloudShadow, v_shadowUv).r * u_cloudShadow.x, 1.0);
    float light  = 0.38
                 + (0.55 * max(dot(n, up), 0.0)
                    + 0.18 * abs(n.z)
                    + 0.08 * dot(n, side)) * (1.0 - shadow);
    vec3 col = v_color0.rgb * mix(light, 1.0, u_material.x);

    float fog = fog_amount(length(v_relPos)) * (1.0 - 0.7 * u_material.x);
    gl_FragColor = vec4(mix(col, u_fogColor.rgb, fog), 1.0);
}
