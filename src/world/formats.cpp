#include "world/formats.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/json.h"
#include "world/wrap.h"

namespace world {

namespace {

std::optional<std::vector<uint8_t>> read_file(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "world: cannot open %s\n", path.c_str());
        return std::nullopt;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(size > 0 ? static_cast<size_t>(size) : 0);
    const size_t got = data.empty() ? 0 : std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (got != data.size()) {
        std::fprintf(stderr, "world: short read on %s\n", path.c_str());
        return std::nullopt;
    }
    return data;
}

// Bounds-checked sequential reader over a file image.
struct Reader
{
    const std::vector<uint8_t>& data;
    size_t                      pos = 0;

    template <typename T>
    bool read(T& out)
    {
        if (data.size() - pos < sizeof(T)) return false;
        std::memcpy(&out, data.data() + pos, sizeof(T));
        pos += sizeof(T);
        return true;
    }

    template <typename T>
    bool read_array(std::vector<T>& out, size_t count)
    {
        if (count > (data.size() - pos) / sizeof(T)) return false;
        out.resize(count);
        if (count) std::memcpy(out.data(), data.data() + pos, count * sizeof(T));
        pos += count * sizeof(T);
        return true;
    }
};

} // namespace

std::optional<Manifest> load_manifest(const std::string& path)
{
    const auto bytes = read_file(path);
    if (!bytes) return std::nullopt;

    const auto doc = json::parse({reinterpret_cast<const char*>(bytes->data()), bytes->size()});
    if (!doc) {
        std::fprintf(stderr, "world: %s is not valid JSON\n", path.c_str());
        return std::nullopt;
    }

    const json::Value& map = (*doc)["map"];
    Manifest m;
    m.map_w   = map["width"].as_double();
    m.map_h   = map["height"].as_double();
    m.chunk_x = map["chunk_x"].as_double();
    m.chunk_y = map["chunk_y"].as_double();
    m.radius  = map["cylinder_radius"].as_double();
    m.columns = map["columns"].as_int();
    m.rows    = map["rows"].as_int();

    if (m.map_w != kMapW) {
        std::fprintf(stderr, "world: manifest map width %.3f != compiled wrap width %.3f\n",
                     m.map_w, kMapW);
        return std::nullopt;
    }
    if (m.columns <= 0 || m.rows <= 0 || m.chunk_x <= 0 || m.chunk_y <= 0 || m.radius <= 0
        || std::abs(m.columns * m.chunk_x - m.map_w) > 1e-6) {
        std::fprintf(stderr, "world: manifest map block is incomplete or inconsistent\n");
        return std::nullopt;
    }

    const json::Value& types = (*doc)["building_types"];
    for (size_t i = 0; i < types.size(); ++i) {
        BuildingType t;
        t.name      = types[i]["name"].as_string();
        t.height    = static_cast<float>(types[i]["height"].as_double());
        t.courtyard = types[i]["courtyard"].as_bool();
        m.building_types.push_back(std::move(t));
    }

    // Material 7 is the generator's MAT_BUILDING; the prototypes carry no colour.
    const json::Value& rgb = (*doc)["materials"]["7"];
    if (rgb.size() != 3) {
        std::fprintf(stderr, "world: manifest has no building material\n");
        return std::nullopt;
    }
    for (size_t k = 0; k < 3; ++k) m.building_rgb[k] = static_cast<float>(rgb[k].as_double());

    const json::Value& chunks = (*doc)["chunks"];
    m.chunks.reserve(chunks.size());
    for (size_t k = 0; k < chunks.size(); ++k) {
        ChunkEntry c;
        c.i    = chunks[k]["i"].as_int(-1);
        c.j    = chunks[k]["j"].as_int(-1);
        c.file = chunks[k]["file"].as_string();
        if (c.i < 0 || c.i >= m.columns || c.j < 0 || c.j >= m.rows || c.file.empty()) {
            std::fprintf(stderr, "world: manifest chunk entry %zu is malformed\n", k);
            return std::nullopt;
        }
        m.chunks.push_back(std::move(c));
    }
    return m;
}

std::optional<ChunkData> load_chunk(const std::string& path)
{
    const auto bytes = read_file(path);
    if (!bytes) return std::nullopt;

    Reader    r{*bytes};
    ChunkData c;
    if (!r.read(c.header) || std::memcmp(c.header.magic, "CHNK", 4) != 0 || c.header.version != 1) {
        std::fprintf(stderr, "world: %s is not a version-1 chunk\n", path.c_str());
        return std::nullopt;
    }
    const ChunkHeader& h = c.header;
    if (!r.read_array(c.vertices, h.vertex_count) || !r.read_array(c.indices, h.index_count)
        || !r.read_array(c.buildings, h.building_count)
        || !r.read_array(c.monuments, h.monument_count)) {
        std::fprintf(stderr, "world: %s is truncated\n", path.c_str());
        return std::nullopt;
    }
    for (const uint32_t idx : c.indices) {
        if (idx >= h.vertex_count) {
            std::fprintf(stderr, "world: %s has an out-of-range index\n", path.c_str());
            return std::nullopt;
        }
    }
    return c;
}

std::optional<PrototypeIndex> load_prototype_index(const std::string& path)
{
    const auto bytes = read_file(path);
    if (!bytes) return std::nullopt;

    const auto doc = json::parse({reinterpret_cast<const char*>(bytes->data()), bytes->size()});
    if (!doc || (*doc)["version"].as_int() != 2
        || (*doc)["vertex_stride"].as_int() != static_cast<int>(sizeof(BuildVertex))) {
        std::fprintf(stderr, "world: %s is not a version-2 prototype index\n", path.c_str());
        return std::nullopt;
    }

    PrototypeIndex x;
    const json::Value& bands = (*doc)["lod_bands"];
    for (size_t k = 0; k < bands.size(); ++k) {
        if (bands[k]["lod"].as_int(-1) != static_cast<int>(k)) {
            std::fprintf(stderr, "world: %s lists LOD bands out of order\n", path.c_str());
            return std::nullopt;
        }
        x.lod_bands.push_back({static_cast<float>(bands[k]["max_distance"].as_double()),
                               bands[k]["openings"].as_bool()});
    }
    x.interior_max_distance = static_cast<float>((*doc)["interior_max_distance"].as_double());
    const json::Value& tile = (*doc)["facade_tile"];
    x.bay         = static_cast<float>(tile["size"].as_double());
    x.window_w    = static_cast<float>(tile["window"]["width"].as_double());
    x.window_h    = static_cast<float>(tile["window"]["height"].as_double());
    x.window_sill = static_cast<float>(tile["window"]["sill"].as_double());
    x.recess      = static_cast<float>(tile["recess"].as_double());
    const json::Value& files = (*doc)["files"];
    x.shell_file     = files["shell"].as_string();
    x.interior_file  = files["interior"].as_string();
    x.collision_file = files["collision"].as_string();
    if (x.lod_bands.empty() || x.bay <= 0 || x.window_w <= 0 || x.window_h <= 0
        || x.shell_file.empty() || x.interior_file.empty() || x.collision_file.empty()) {
        std::fprintf(stderr, "world: %s is incomplete\n", path.c_str());
        return std::nullopt;
    }
    return x;
}

std::optional<std::vector<PrototypeMesh>> load_prototypes(const std::string& path)
{
    const auto bytes = read_file(path);
    if (!bytes) return std::nullopt;

    Reader   r{*bytes};
    char     magic[4];
    uint32_t version = 0, count = 0;
    if (!r.read(magic) || std::memcmp(magic, "PROT", 4) != 0 || !r.read(version) || version != 2
        || !r.read(count)) {
        std::fprintf(stderr, "world: %s is not a version-2 prototype file\n", path.c_str());
        return std::nullopt;
    }

    // The 14-byte mesh header leaves everything after it unaligned; Reader copies
    // field by field, so that is fine.
    std::vector<PrototypeMesh> meshes(count);
    for (PrototypeMesh& m : meshes) {
        uint32_t nv = 0, ni = 0;
        if (!r.read(m.type) || !r.read(m.lod) || !r.read(m.part) || !r.read(nv) || !r.read(ni)
            || !r.read_array(m.vertices, nv) || !r.read_array(m.indices, ni)) {
            std::fprintf(stderr, "world: %s is truncated\n", path.c_str());
            return std::nullopt;
        }
        for (const uint32_t idx : m.indices) {
            if (idx >= nv) {
                std::fprintf(stderr, "world: %s has an out-of-range index\n", path.c_str());
                return std::nullopt;
            }
        }
    }
    return meshes;
}

} // namespace world
