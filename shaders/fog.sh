// Haze shared by every world fragment shader.
uniform vec4 u_fog;         // x: density per metre, y: opaque distance, z: clear distance
uniform vec4 u_fogColor;

// Exponential haze rescaled so it reaches exactly 1 at the opaque distance; a bare
// exponential only approaches it.
float fog_amount(float dist)
{
    float span = u_fog.y - u_fog.z;
    float fog  = (1.0 - exp(-max(dist - u_fog.z, 0.0) * u_fog.x))
               / (1.0 - exp(-span * u_fog.x));
    return min(fog, 1.0);
}
