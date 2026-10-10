// Light from the central column, shared by every lit world fragment shader.
//
// The column is a line light along the axis. Faces toward the axis get most of
// it, faces looking along the axis catch its oblique run, and faces looking
// around the circumference see it edge-on and get mostly ambient. The small
// tangent term only separates the two circumferential wall directions. Cloud
// shadow (0..1, the fraction of the column hidden) dims the column's share, not
// the ambient.
float column_light(vec3 n, vec3 up, float shadow)
{
    vec3 side = cross(up, vec3(0.0, 0.0, 1.0));
    return 0.38
         + (0.55 * max(dot(n, up), 0.0)
            + 0.18 * abs(n.z)
            + 0.08 * dot(n, side)) * (1.0 - shadow);
}
