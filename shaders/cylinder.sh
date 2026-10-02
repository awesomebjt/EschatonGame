// Cylindrical projection shared by every world vertex shader.
//
// Input is map space relative to the camera: x around the circumference (already
// wrapped to the shortest arc), y along the axis, z up off the wall. Output is
// render space with the camera at the origin and always at theta = 0:
//   +x  tangent to the circumference (map +X)
//   +y  toward the rotation axis (map +Z, "up")
//   +z  along the axis (map +Y)
// Because the camera sits at theta = 0 every frame, moving around the cylinder
// never accumulates large render-space coordinates; the wrap is free.

uniform vec4 u_cylinder;    // x: radius R, y: camera altitude above the wall

vec3 cyl_position(vec3 m, out vec3 tangent, out vec3 up)
{
    float R     = u_cylinder.x;
    float theta = m.x / R;
    float r     = R - m.z;
    float s     = sin(theta);
    float c     = cos(theta);
    float sh    = sin(0.5 * theta);
    tangent = vec3(c, s, 0.0);
    up      = vec3(-s, c, 0.0);
    // Height relative to the camera is r(1 - cos θ) + (z - alt); writing 1 - cos θ
    // as 2 sin²(θ/2) avoids cancelling two ~4 km values near the viewer.
    return vec3(r * s, m.z - u_cylinder.y + 2.0 * r * sh * sh, m.y);
}

// Map-space direction (x around, y along, z up) at a point on the cylinder.
vec3 cyl_direction(vec3 d, vec3 tangent, vec3 up)
{
    return d.x * tangent + d.z * up + vec3(0.0, 0.0, d.y);
}
