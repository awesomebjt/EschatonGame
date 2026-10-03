#include "world/clouds.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace world {

namespace {

// splitmix64: tiny, seedable, and the same on every platform (unlike <random>'s
// distributions), so a seed always gives the same sky.
struct Rng
{
    uint64_t s;
    uint64_t next_u64()
    {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double uniform() { return (next_u64() >> 11) * (1.0 / 9007199254740992.0); }
    double range(double lo, double hi) { return lo + (hi - lo) * uniform(); }
};

constexpr double k_min_width = 300.0, k_width_span = 600.0;

// Rough footprint of a generated cloud of width w: an ellipse ~0.8 as deep as it
// is wide, a little lumpy. Only used to turn coverage into a cloud count;
// physics_check measures the real coverage.
double footprint_area(double w) { return std::numbers::pi * 0.25 * w * w * 0.8 * 0.9; }

void build_puffs(Rng& rng, double width, const CloudParams& p, Cloud& c, std::vector<Puff>& out)
{
    c.first_puff = static_cast<uint32_t>(out.size());
    const double a = 0.5 * width;                       // semi-axis around the circumference
    const double b = a * rng.range(0.65, 0.95);         // along the axis
    const double target_h = width * rng.range(0.4, 0.85);
    float top = 0;

    // Base layer: wide puffs whose centres sit just above the base, so the flat
    // cut in the shader leaves a level bottom.
    const int n_base = 5 + static_cast<int>(width / 100.0);
    for (int k = 0; k < n_base; ++k) {
        const double r   = width * rng.range(0.16, 0.26);
        const double ang = rng.range(0, 2 * std::numbers::pi);
        const double rad = std::sqrt(rng.uniform());
        const double x   = std::max(0.0, a - r) * rad * std::cos(ang);
        const double y   = std::max(0.0, b - r) * rad * std::sin(ang);
        const double z   = 0.55 * r;
        out.push_back({static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(r)});
        top = std::max(top, static_cast<float>(z + r));
    }

    // Tower: shrinking clusters stacked up the middle, each level shifted spinward.
    double rc = width * 0.3;
    double z  = 0.7 * rc;
    while (z + rc < target_h && rc > 25.0) {
        const int n = 2 + static_cast<int>(rng.uniform() * 3);
        for (int k = 0; k < n; ++k) {
            const double r = rc * rng.range(0.7, 1.0);
            const double x = p.lean * z + rc * rng.range(-0.35, 0.35);
            const double y = rc * rng.range(-0.35, 0.35);
            out.push_back({static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(r)});
            top = std::max(top, static_cast<float>(z + r));
        }
        rc *= 0.82;
        z  += rc * 0.9;
    }
    c.puff_count = static_cast<uint32_t>(out.size()) - c.first_puff;
    c.height     = top;
}

} // namespace

CloudField generate_clouds(const CloudParams& p, double map_w, double map_h, double radius)
{
    CloudField f;
    if (p.coverage <= 0) return f;
    Rng rng{0x5EEDC10Dull ^ (uint64_t(p.seed) << 32) ^ p.seed};

    // Jittered grid: even enough to read as "scattered", never clumped into one
    // overcast patch. Cell counts divide the map exactly so the field wraps.
    const double mean_w = k_min_width + k_width_span * 0.4;   // E[u^1.5] = 0.4
    // Coverage is of the floor, and a cloud's floor footprint is stretched around
    // the circumference by the radial projection.
    const double count  = p.coverage * map_w * map_h
                        / (footprint_area(mean_w) * cloud_x_scale(radius, p.base_alt));
    const double cell   = std::sqrt(map_w * map_h / std::max(1.0, count));
    const int    nx     = std::max(1, static_cast<int>(std::lround(map_w / cell)));
    const int    ny     = std::max(1, static_cast<int>(std::lround(map_h / cell)));
    const double cw = map_w / nx, ch = map_h / ny;

    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            Cloud c;
            c.x     = (i + rng.range(0.1, 0.9)) * cw;
            c.y     = (j + rng.range(0.1, 0.9)) * ch;
            c.base  = static_cast<float>(p.base_alt + rng.range(-80.0, 80.0));
            c.shade = static_cast<float>(rng.uniform());
            const double width = k_min_width + k_width_span * std::pow(rng.uniform(), 1.5);
            build_puffs(rng, width, p, c, f.puffs);
            f.clouds.push_back(c);
        }
    }
    return f;
}

std::vector<uint8_t> cloud_shadow_map(const CloudField& field, const CloudParams& p, int width, int height,
                                      double map_w, double map_h, double radius)
{
    const double tw = map_w / width, th = map_h / height;   // texel size, m

    // Footprint: how opaque the column of air above each texel is.
    std::vector<float> foot(static_cast<size_t>(width) * height, 0.0f);
    for (const Cloud& c : field.clouds) {
        for (uint32_t k = 0; k < c.puff_count; ++k) {
            const Puff&  pf = field.puffs[c.first_puff + k];
            const double sx = cloud_x_scale(radius, c.base + pf.z);
            const double px = c.x + pf.x * sx, py = c.y + pf.y, r = pf.radius, rx = r * sx;
            const int x0 = static_cast<int>(std::floor((px - rx) / tw)), x1 = static_cast<int>(std::ceil((px + rx) / tw));
            const int y0 = static_cast<int>(std::floor((py - r) / th)), y1 = static_cast<int>(std::ceil((py + r) / th));
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    const double d = std::hypot(((x + 0.5) * tw - px) / sx, (y + 0.5) * th - py) / r;
                    if (d >= 1.0) continue;
                    // Soft rim: puffs are spheres, thinner toward their edges.
                    const float a = static_cast<float>(0.95 * std::min(1.0, (1.0 - d) / 0.3));
                    const size_t idx = static_cast<size_t>(((y % height) + height) % height) * width
                                     + ((x % width) + width) % width;
                    foot[idx] = std::max(foot[idx], a);
                }
            }
        }
    }

    // Convolve along the axis with the line-light kernel (see the header).
    const double h_eff = p.base_alt + 150.0;   // representative height of the layer
    const int    reach = static_cast<int>(std::ceil(4.0 * h_eff / th));
    std::vector<float> kernel(2 * reach + 1);
    for (int j = -reach; j <= reach; ++j) {
        const double u = j * th / h_eff;
        kernel[j + reach] = static_cast<float>(2.0 / std::numbers::pi / ((1 + u * u) * (1 + u * u)) * th / h_eff);
    }

    std::vector<uint8_t> out(foot.size(), 0);
    std::vector<float>   column(height);
    for (int x = 0; x < width; ++x) {
        for (int y = 0; y < height; ++y) column[y] = foot[static_cast<size_t>(y) * width + x];
        for (int y = 0; y < height; ++y) {
            float s = 0;
            for (int j = -reach; j <= reach; ++j) {
                const float f = column[((y + j) % height + height) % height];
                if (f > 0) s += f * kernel[j + reach];
            }
            out[static_cast<size_t>(y) * width + x] = static_cast<uint8_t>(std::lround(255.0f * std::min(1.0f, s)));
        }
    }
    return out;
}

} // namespace world
