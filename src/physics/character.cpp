#include "physics/character.h"

#include <algorithm>
#include <cmath>

#include "world/wrap.h"

namespace physics {

namespace {

struct V3
{
    float x, y, z;
};

V3    operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3    operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3    operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3    cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(V3 a) { return std::sqrt(dot(a, a)); }
V3    load(const float* p) { return {p[0], p[1], p[2]}; }

// Closest point on triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5).
V3 closest_on_triangle(V3 p, V3 a, V3 b, V3 c)
{
    const V3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    const V3 bp = p - b;
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
    const V3 cp = p - c;
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

// Closest points between segments p1q1 and p2q2 (Ericson 5.1.9).
void closest_segments(V3 p1, V3 q1, V3 p2, V3 q2, V3& c1, V3& c2)
{
    const V3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    float s = 0, t = 0;
    constexpr float eps = 1e-8f;
    if (a <= eps && e <= eps) {
        s = t = 0;
    } else if (a <= eps) {
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = dot(d1, r);
        if (e <= eps) {
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = dot(d1, d2), denom = a * e - b * b;
            s = denom != 0 ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0) {
                t = 0;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1) {
                t = 1;
                s = std::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    c1 = p1 + d1 * s;
    c2 = p2 + d2 * t;
}

struct Contact
{
    V3    normal;   // out of the surface, toward the capsule
    float depth;
};

// Penetration of the capsule (segment s0s1, radius r) into triangle abc, if any.
bool capsule_triangle(V3 s0, V3 s1, float r, const Tri& t, Contact& out)
{
    const V3 a = load(t.a), b = load(t.b), c = load(t.c);
    V3 n = cross(b - a, c - a);
    const float area = length(n);
    if (area < 1e-8f) return false;   // degenerate sliver
    n = n * (1.0f / area);

    // Does the segment pass through the triangle? Then the face normal decides.
    const float d0 = dot(s0 - a, n), d1 = dot(s1 - a, n);
    if ((d0 < 0) != (d1 < 0)) {
        const V3 hit = s0 + (s1 - s0) * (d0 / (d0 - d1));
        if (length(closest_on_triangle(hit, a, b, c) - hit) < 1e-4f) {
            // Push toward the side the capsule's middle is on, far enough that the
            // end poking through clears the face by the radius.
            float e0 = d0, e1 = d1;
            if (d0 + d1 < 0) {
                n  = n * -1.0f;
                e0 = -d0;
                e1 = -d1;
            }
            out = {n, r - std::min(e0, e1)};
            return true;
        }
    }

    // Otherwise the closest pair is an endpoint against the face, or an edge
    // against the segment.
    V3 best_s = s0, best_t = closest_on_triangle(s0, a, b, c);
    float best = dot(best_t - best_s, best_t - best_s);
    auto consider = [&](V3 ps, V3 pt) {
        const float d = dot(pt - ps, pt - ps);
        if (d < best) { best = d; best_s = ps; best_t = pt; }
    };
    consider(s1, closest_on_triangle(s1, a, b, c));
    const V3 edges[3][2] = {{a, b}, {b, c}, {c, a}};
    for (const auto& e : edges) {
        V3 ps, pt;
        closest_segments(s0, s1, e[0], e[1], ps, pt);
        consider(ps, pt);
    }
    if (best >= r * r) return false;
    const float dist = std::sqrt(best);
    if (dist > 1e-5f) {
        out = {(best_s - best_t) * (1.0f / dist), r - dist};
    } else {
        const float dm = 0.5f * (d0 + d1);
        out = {dm < 0 ? n * -1.0f : n, r};
    }
    return true;
}

struct Mover
{
    const std::vector<Tri>& tris;
    float                   radius, height, max_slope;

    bool touched_ground  = false;
    V3   ground_normal   = {0, 0, 1};
    bool touched_ceiling = false;

    // Pushes p out of everything it overlaps, deepest first, a few rounds.
    // Walkable surfaces push straight up so standing on a ramp doesn't slide.
    // Steep contacts clip `v` so it slides along walls instead of into them.
    void resolve(V3& p, float v[3])
    {
        for (int iter = 0; iter < 6; ++iter) {
            const V3 s0 = p + V3{0, 0, radius};
            const V3 s1 = p + V3{0, 0, height - radius};
            Contact deepest{{0, 0, 0}, 0};
            for (const Tri& t : tris) {
                Contact c;
                if (capsule_triangle(s0, s1, radius, t, c) && c.depth > deepest.depth) deepest = c;
            }
            if (deepest.depth <= 1e-5f) return;

            const V3 n = deepest.normal;
            if (n.z >= max_slope) {
                p.z += deepest.depth / n.z;
                touched_ground = true;
                ground_normal  = n;
            } else {
                p = p + n * deepest.depth;
                if (n.z < -0.5f) touched_ceiling = true;
                const V3    vel{v[0], v[1], v[2]};
                const float into = dot(vel, n);
                if (into < 0) {
                    v[0] -= n.x * into;
                    v[1] -= n.y * into;
                    v[2] -= n.z * into;
                }
            }
        }
    }
};

} // namespace

void step_character(CharacterBody& body, const MoveIntent& intent, const CapsuleShape& shape,
                    const CharacterParams& params, const RotatingFrame& frame,
                    const CollisionWorld& world, double map_h, double dt, std::vector<Tri>& scratch)
{
    const float fdt = static_cast<float>(dt);

    // -- Velocity: input, jump, pseudoforces ------------------------------------
    const float speed  = intent.sprint ? params.sprint_speed : params.walk_speed;
    const float want_x = intent.wish_x * speed, want_y = intent.wish_y * speed;
    const float accel  = (body.grounded ? params.ground_accel : params.air_accel) * fdt;
    const float dx = want_x - body.v[0], dy = want_y - body.v[1];
    const float gap = std::hypot(dx, dy);
    // In the air, only steering acts: braking toward a zero wish would cancel the
    // Coriolis drift that a fall or a long jump is supposed to show.
    const bool steering = body.grounded || intent.wish_x != 0 || intent.wish_y != 0;
    if (gap > 0 && steering) {
        const float k = std::min(1.0f, accel / gap);
        body.v[0] += dx * k;
        body.v[1] += dy * k;
    }
    bool jumped = false;
    if (intent.jump && body.grounded) {
        body.v[2] = params.jump_speed;
        jumped    = true;
    }
    double a[3];
    frame.acceleration(body.alt, body.v, a);
    for (int k = 0; k < 3; ++k) body.v[k] += static_cast<float>(a[k] * dt);

    // Local metres to map metres: circumferential distance scales with radius.
    const double R       = frame.radius;
    const float  x_scale = static_cast<float>(R / std::max(1.0, R - body.alt));
    const V3     d{body.v[0] * fdt * x_scale, body.v[1] * fdt, body.v[2] * fdt};

    // -- Collide in a small frame around the feet ---------------------------------
    const float reach = length(d) + shape.radius + params.step_height + params.snap + 1.0f;
    scratch.clear();
    world.gather(body.x, body.y, body.alt, reach, scratch);

    Mover mover{scratch, shape.radius, shape.height, params.max_slope};
    V3    p{0, 0, 0};
    const bool was_grounded = body.grounded && !jumped;

    // Step up first, so the horizontal pass clears anything up to step_height.
    float lift = 0;
    if (was_grounded && (d.x != 0 || d.y != 0)) {
        p.z += params.step_height;
        mover.resolve(p, body.v);
        lift = p.z;
    }

    // Horizontal, in sub-steps of half a radius so fast moves can't tunnel.
    const float horiz = std::hypot(d.x, d.y);
    const int   hsteps = std::clamp(static_cast<int>(std::ceil(horiz / (0.5f * shape.radius))), 1, 32);
    for (int k = 0; k < hsteps; ++k) {
        p.x += d.x / hsteps;
        p.y += d.y / hsteps;
        mover.resolve(p, body.v);
    }

    // Vertical: undo the lift, apply the frame's own vertical motion, and when
    // walking reach a little further to stay glued to ramps and kerbs.
    const float z_after_horizontal = p.z;
    const float vertical = -lift + d.z;
    const float snap     = was_grounded ? params.snap : 0.0f;
    if (vertical > 0) {
        const int up_steps = std::clamp(static_cast<int>(std::ceil(vertical / (0.5f * shape.radius))), 1, 64);
        for (int k = 0; k < up_steps; ++k) {
            p.z += vertical / up_steps;
            mover.resolve(p, body.v);
        }
    }
    mover.touched_ground = false;
    const float drop = std::max(0.0f, -vertical) + snap;
    if (drop > 0) {
        const int down_steps = std::clamp(static_cast<int>(std::ceil(drop / (0.5f * shape.radius))), 1, 64);
        for (int k = 0; k < down_steps && !mover.touched_ground; ++k) {
            p.z -= drop / down_steps;
            mover.resolve(p, body.v);
        }
        // Snapping found nothing: walked off an edge. Fall from where free motion
        // would have put us rather than from the bottom of the snap probe.
        if (!mover.touched_ground && snap > 0) {
            p.z = z_after_horizontal + vertical;
            mover.resolve(p, body.v);
        }
    }

    body.grounded = mover.touched_ground && body.v[2] <= 0.0f;
    if (body.grounded) {
        body.v[2] = 0;
        body.ground_normal[0] = mover.ground_normal.x;
        body.ground_normal[1] = mover.ground_normal.y;
        body.ground_normal[2] = mover.ground_normal.z;
    }
    if (mover.touched_ceiling && body.v[2] > 0) body.v[2] = 0;

    // -- Commit -----------------------------------------------------------------
    body.x   = world::wrap_x(body.x + p.x);
    body.y  += p.y;
    body.alt += p.z;

    // The end caps aren't built yet: hold the player inside the axial extent.
    const double lo = shape.radius, hi = map_h - shape.radius;
    if (body.y < lo || body.y > hi) {
        body.y    = std::clamp(body.y, lo, hi);
        body.v[1] = 0;
    }
    // Never past the light column.
    if (body.alt > R - 100.0) {
        body.alt  = R - 100.0;
        body.v[2] = std::min(body.v[2], 0.0f);
    }
}

float capsule_penetration(const std::vector<Tri>& tris, const CapsuleShape& shape)
{
    const V3 s0{0, 0, shape.radius}, s1{0, 0, shape.height - shape.radius};
    float worst = 0;
    for (const Tri& t : tris) {
        Contact c;
        if (capsule_triangle(s0, s1, shape.radius, t, c)) worst = std::max(worst, c.depth);
    }
    return worst;
}

} // namespace physics
