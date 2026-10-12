$input v_color0, v_normal, v_up, v_relPos, v_tangent

#include <bgfx_shader.sh>
#include "fog.sh"
#include "lighting.sh"

// Flat player colour under the column light, with a dark visor band at eye
// height on the front so facing reads at a glance.
void main()
{
    vec3 n  = normalize(v_normal);
    vec3 up = normalize(v_up);
    vec3 col = v_color0.rgb * column_light(n, up, 0.0);

    vec3  lp    = v_tangent;
    float front = lp.y / max(length(lp.xy), 1e-4);
    if (lp.z > 1.48 && lp.z < 1.70 && front > 0.35)
        col = vec3(0.05, 0.06, 0.08) + 0.25 * max(dot(n, up), 0.0);

    float fog = fog_amount(length(v_relPos));
    gl_FragColor = vec4(mix(col, u_fogColor.rgb, fog), 1.0);
}
