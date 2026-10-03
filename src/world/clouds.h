#pragma once

#include <cstdint>
#include <vector>

namespace world {

// Scattered cumulus, generated deterministically from a seed. Clouds wrap in both
// map axes so a drifting field never runs out.
//
// Air in the habitat is expected to drift spinward aloft: rising air keeps the
// angular momentum of the floor it left, which is more than the frame's at a
// smaller radius (the habitat's version of Earth's Hadley cell). Updrafts lean the
// same way, so towers are built leaning spinward. See src/render/CLAUDE.md.
struct CloudParams
{
    uint32_t seed      = 1;
    float    coverage  = 0.12f;    // fraction of the floor under a cloud
    float    base_alt  = 1500.0f;  // m above the floor
    float    lean      = 0.25f;    // spinward offset per metre of height
    float    wind_x    = 5.0f;     // m/s, spinward
    float    wind_y    = 0.0f;     // m/s, along the axis
};

struct Puff
{
    float x, y, z;   // metres from the cloud's base centre (true size, not map x); z up
    float radius;
};

struct Cloud
{
    double   x = 0, y = 0;      // map position of the base centre
    float    base = 0;          // base altitude
    float    height = 0;        // base to top
    float    shade = 0;         // per-cloud brightness seed in [0, 1)
    uint32_t first_puff = 0, puff_count = 0;
};

struct CloudField
{
    std::vector<Cloud> clouds;
    std::vector<Puff>  puffs;
};

CloudField generate_clouds(const CloudParams& params, double map_w, double map_h, double radius);

// Fraction of the light column each floor texel loses to the clouds, as an R8
// map over the whole floor (u = x / map_w, v = y / map_h, both repeating).
//
// The column is a line along the axis, so the shadow is sharp around the
// circumference but smeared along the axis: a floor point sees the column over
// ±90°, and with u = axial offset / height the irradiance per du is
// (2/π) / (1 + u²)². Each texel column of cloud footprint is convolved with that
// kernel. A 600 m cloud at 1.5 km blocks ~24% directly beneath it.
//
// Light travels radially, so a puff's shadow is its own outline projected out to
// the floor: R / (R − alt) wider around the circumference (~1.6× at 1.5 km),
// unchanged along the axis.
std::vector<uint8_t> cloud_shadow_map(const CloudField& field, const CloudParams& params, int width,
                                      int height, double map_w, double map_h, double radius);

// Puff offsets are true metres at the cloud's own radius; map x is floor metres.
inline double cloud_x_scale(double radius, double alt) { return radius / (radius - alt); }

} // namespace world
