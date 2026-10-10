$input v_color0, v_normal, v_up, v_relPos, v_shadowUv, v_texcoord0, v_tangent

#include <bgfx_shader.sh>
#include "fog.sh"
#include "lighting.sh"

// Building surfaces. UVs are metric (u along the wall, v up from the base) and
// every wall is phased so its bay centres sit at u = bay/2 (mod bay), which is
// what lets the windows redrawn on coarse LODs land on the real openings.
uniform vec4 u_material;      // y: mode, see world_renderer.cpp (masonry, parallax, flat, glazing, interior)
uniform vec4 u_facade;        // x: bay, y: window width, z: window height incl. arch, w: sill
uniform vec4 u_facadeColor;   // rgb: building material, w: opening recess depth
uniform vec4 u_cloudShadow;   // x: shadow strength (0 = off)
SAMPLER2D(s_cloudShadow, 0);

float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Signed distance to the arched window opening, in metres within one bay.
float window_sdf(vec2 p)
{
    float hw     = 0.5 * u_facade.y;
    float spring = u_facade.w + u_facade.z - hw;
    vec2  q      = vec2(p.x - 0.5 * u_facade.x, p.y);
    vec2  d      = abs(q - vec2(0.0, 0.5 * (u_facade.w + spring))) - vec2(hw, 0.5 * (spring - u_facade.w));
    float box    = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
    float arch   = length(q - vec2(0.0, spring)) - hw;
    return min(box, arch);
}

// Window coverage at uv, antialiased over `aa` metres. Once a bay shrinks to a
// few pixels the pattern can only alias, so it fades to its average coverage.
float window_mask(vec2 uv, float aa)
{
    float bay    = u_facade.x;
    float hw     = 0.5 * u_facade.y;
    float spring = u_facade.w + u_facade.z - hw;
    float mean   = (2.0 * hw * (spring - u_facade.w) + 1.5708 * hw * hw) / (bay * bay);
    float m      = 1.0 - smoothstep(-aa, aa, window_sdf(mod(uv, bay)));
    return mix(m, mean, smoothstep(0.08 * bay, 0.3 * bay, aa));
}

// Sintered-regolith brick: running bond, faint mortar, gone once it would shimmer.
vec3 masonry(vec3 base, vec2 uv, float aa)
{
    float course = floor(uv.y / 0.077);
    float bu     = mod(uv.x + mod(course, 2.0) * 0.105, 0.21);
    float bv     = mod(uv.y, 0.077);
    float edge   = min(min(bu, 0.21 - bu), min(bv, 0.077 - bv));
    float mortar = 1.0 - smoothstep(0.006 - aa, 0.006 + aa, edge);
    float fade   = 1.0 - smoothstep(0.004, 0.02, aa);
    float tone   = 1.0 + 0.06 * (hash12(vec2(floor((uv.x + mod(course, 2.0) * 0.105) / 0.21), course)) - 0.5);
    return base * mix(1.0, tone * (1.0 - 0.14 * mortar), fade);
}

// The room behind a window, ray-cast as a box one bay wide, a storey high
// (less the slab) and `depth` deep. Lit from the window, so it darkens inward.
vec3 room(vec2 uv, vec3 dir, float seed)
{
    float bay   = u_facade.x;
    float depth = 6.0;
    float ceil_ = bay - 0.5;
    vec2  cell  = floor(uv / bay);
    vec2  p     = uv - cell * bay;
    float h     = hash12(cell + seed * 97.0);

    // Keep every component off zero so the slab distances below stay finite.
    vec3 rd = vec3(dir.x >= 0.0 ? max(dir.x, 1e-4) : min(dir.x, -1e-4),
                   dir.y >= 0.0 ? max(dir.y, 1e-4) : min(dir.y, -1e-4),
                   max(dir.z, 1e-3));
    vec3 inv = 1.0 / rd;
    float tx = (rd.x > 0.0 ? bay - p.x : -p.x) * inv.x;
    float ty = (rd.y > 0.0 ? ceil_ - p.y : -p.y) * inv.y;
    float tz = depth * inv.z;
    float t  = min(min(tx, ty), tz);
    vec3 hit = vec3(p, 0.0) + rd * t;

    vec3 wall = mix(vec3(0.72, 0.66, 0.56), vec3(0.58, 0.64, 0.66), h);
    vec3 col  = wall;
    if (t == ty) col = rd.y > 0.0 ? vec3(0.88, 0.86, 0.82) : vec3(0.42, 0.30, 0.20);
    else if (t == tx) col = wall * (rd.x > 0.0 ? 0.70 : 0.82);   // light comes in at an angle
    // Lit through the window, so the room darkens toward the back.
    float lit = mix(1.0, 0.45, hit.z / depth);
    // Some rooms have their blinds down.
    float blind = step(0.82, h);
    return mix(col * lit * 0.8, vec3(0.78, 0.72, 0.60) * 0.6, blind);
}

void main()
{
    vec3  n    = normalize(v_normal);
    vec3  up   = normalize(v_up);
    vec3  t    = normalize(v_tangent);
    vec2  uv   = v_texcoord0;
    float mode = u_material.y;
    vec3  view = normalize(v_relPos);   // camera to fragment

    float shadow = min(texture2D(s_cloudShadow, v_shadowUv).r * u_cloudShadow.x, 1.0);
    float light  = column_light(n, up, shadow);
    float aa     = max(length(fwidth(uv)), 1e-4);
    vec3  base   = v_color0.rgb;
    // Window glass seen from far off: the average of what the rooms show.
    vec3  glass  = vec3(0.17, 0.17, 0.18);
    vec3  col;

    if (mode > 3.5) {
        // Interior: plain plaster, lit like everything else.
        col = vec3(0.80, 0.77, 0.71) * light;
    } else if (mode > 2.5) {
        // Glazing: the view ray in the wall's frame (x along, y up, z into the room).
        vec3 dir  = vec3(dot(view, t), dot(view, up), -dot(view, n));
        col = room(uv, dir, v_color0.a) * (0.6 + 0.4 * light);
        float fres = 0.04 + 0.96 * pow(1.0 - clamp(dir.z, 0.0, 1.0), 5.0);
        col = mix(col, u_fogColor.rgb * 0.8, 0.5 * fres);
    } else {
        col = masonry(base, uv, aa);
        // Walls only: roofs and slabs carry plan uvs, not facade uvs.
        bool wall = abs(dot(n, up)) < 0.5;
        if (mode > 0.5 && wall) {
            float hole = window_mask(uv, aa);
            if (mode < 1.5) {
                // Parallax: a fragment inside an opening sees glass if its ray is
                // still inside the opening at the recess depth, otherwise the reveal.
                float vn   = max(-dot(view, n), 0.05);
                vec2  off  = vec2(dot(view, t), dot(view, up)) * (u_facadeColor.w / vn);
                float back = window_mask(uv + off, aa);
                col = mix(col, mix(col * 0.62, glass, back), hole);
            } else {
                col = mix(col, glass, hole);
            }
        }
        col *= light;
    }

    float fog = fog_amount(length(v_relPos));
    gl_FragColor = vec4(mix(col, u_fogColor.rgb, fog), 1.0);
}
