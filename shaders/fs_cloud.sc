$input v_texcoord0, v_cloud, v_upView, v_relPos, v_cloudAux

#include <bgfx_shader.sh>
#include "fog.sh"

// v_cloud:    x cloud base relative to the puff centre (m, along up; negative),
//             y puff radius, z cloud height, w cloud shade seed
// v_cloudAux: xyz puff centre in view space, w opacity (fades puffs near the eye)
// v_relPos:   this fragment's view-space position on the quad; the ray is from the
//             eye (the origin) through it

float hash12(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float value_noise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1.0, 0.0)), f.x),
               mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), f.x), f.y);
}

void main()
{
    vec3  dir = normalize(v_relPos);
    vec3  c   = v_cloudAux.xyz;
    float r   = v_cloud.y;
    vec3  upv = normalize(v_upView);

    // Ray against the sphere.
    float b  = dot(c, dir);
    vec3  cp = dir * b - c;
    float q2 = dot(cp, cp) / (r * r);   // closest approach, in radii
    if (q2 >= 1.0) discard;
    float half_chord = sqrt(1.0 - q2) * r;
    float t0 = max(b - half_chord, 0.0);
    float t1 = b + half_chord;

    // Cumulus are flat underneath: the puff is the part of the sphere above the
    // cloud base. Where the ray meets the sphere below the base, it may still
    // enter the kept part through the base itself, and then it sees the flat
    // underside. Without this, puffs seen from below are rings and crescents.
    float base  = v_cloud.x;
    float rise  = dot(dir, upv);
    float above = dot(dir * t0 - c, upv) - base;   // height of the entry over the base
    // Seen edge-on the base would be a ruler-straight line; soften it there only,
    // so looking up at the underside it stays solid.
    float steep = smoothstep(0.02, 0.3, rise);
    float base_fade;
    vec3  n;
    if (above >= 0.0) {
        n = (dir * t0 - c) / r;
        base_fade = mix(smoothstep(0.0, 0.12 * r, above), 1.0, steep);
    } else {
        if (rise <= 0.0) discard;                  // heading down, away from the kept part
        if (t0 - above / rise > t1) discard;       // leaves the sphere before the base
        n     = -upv;
        above = 0.0;
        base_fade = smoothstep(0.0, 0.15, rise);
    }

    // Soft, slightly ragged rim by how near the ray passes the sphere's edge.
    float ragged = value_noise(v_texcoord0 * 2.7 + vec2(r * 0.37, base * 0.013));
    float alpha  = smoothstep(1.0, 0.62 + 0.12 * ragged, sqrt(q2));
    // Nearly opaque, so overlapping puffs under one cloud read as one surface
    // rather than showing their circles.
    alpha *= 0.98 * v_cloudAux.w * base_fade;
    if (alpha < 0.004) discard;

    // Lit from the column (toward the axis): bright crowns, grey flat bases.
    // Anything facing down gets only ambient, all alike, so the flat base and the
    // undersides of neighbouring puffs meet without seams.
    float facing = dot(n, upv);
    float lit    = 0.74 + 0.26 * max(facing, 0.0);
    float t      = clamp(above / max(v_cloud.z, 1.0), 0.0, 1.0);
    vec3  base_c = vec3(0.70, 0.73, 0.79);
    vec3  top_c  = vec3(1.00, 0.985, 0.95);
    vec3  col    = mix(base_c, top_c, smoothstep(0.0, 0.6, t)) * lit * (0.94 + 0.08 * v_cloud.w);

    col = mix(col, u_fogColor.rgb, fog_amount(length(c)));
    gl_FragColor = vec4(col, alpha);
}
