// Headless checks for the cloud field: real floor coverage against the target, and
// the shadow map's strength under a cloud against the line-light estimate.
//
//   ./build/clouds_check [out.pgm]   (optionally writes the shadow map to look at)

#include <algorithm>
#include <cstdio>
#include <vector>

#include "world/clouds.h"

int main(int argc, char** argv)
{
    // Canonical map constants (root CLAUDE.md); the checks don't need the world.
    const double map_w = 25100.0, map_h = 32000.0, radius = 3994.789;
    const world::CloudParams p;
    const world::CloudField  f = world::generate_clouds(p, map_w, map_h, radius);

    const int w = 512, h = 653;
    const std::vector<uint8_t> shadow = world::cloud_shadow_map(f, p, w, h, map_w, map_h, radius);

    // Footprint coverage: shadow map without the axial blur is what "under a cloud"
    // means, so measure it with a zero-height light (no smear) by rasterizing again.
    world::CloudParams flat = p;
    flat.base_alt = 1e-3f;   // kernel collapses to (nearly) a delta
    const std::vector<uint8_t> foot = world::cloud_shadow_map(f, flat, w, h, map_w, map_h, radius);
    size_t covered = 0;
    for (uint8_t v : foot) covered += v > 64;
    const double coverage = double(covered) / foot.size();

    size_t sum = 0;
    uint8_t peak = 0;
    for (uint8_t v : shadow) {
        sum += v;
        peak = std::max(peak, v);
    }
    std::printf("%zu clouds, %zu puffs\n", f.clouds.size(), f.puffs.size());
    std::printf("floor coverage %.3f (target %.3f)\n", coverage, p.coverage);
    std::printf("shadow mean %.3f, peak %.3f\n", sum / 255.0 / shadow.size(), peak / 255.0);

    int fails = 0;
    if (coverage < 0.7 * p.coverage || coverage > 1.3 * p.coverage) {
        std::printf("FAIL coverage off target\n");
        ++fails;
    }
    if (peak / 255.0 < 0.15 || peak / 255.0 > 0.6) {
        std::printf("FAIL peak shadow outside 0.15..0.6\n");
        ++fails;
    }

    if (argc > 1) {
        if (FILE* out = std::fopen(argv[1], "wb")) {
            std::fprintf(out, "P5\n%d %d\n255\n", w, h);
            // Stretch for viewing: peak → white.
            for (uint8_t v : shadow) std::fputc(peak ? v * 255 / peak : 0, out);
            std::fclose(out);
        }
    }
    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
