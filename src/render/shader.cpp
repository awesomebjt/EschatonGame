#include "render/shader.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace render {

namespace {

// Map bgfx renderer type to the subdirectory name used by bgfx_compile_shaders.
const char* shader_backend()
{
    switch (bgfx::getRendererType())
    {
        case bgfx::RendererType::Vulkan:     return "spirv";
        case bgfx::RendererType::OpenGL:     return "glsl";
        case bgfx::RendererType::OpenGLES:   return "essl";
        case bgfx::RendererType::Direct3D11: return "dx11";
        case bgfx::RendererType::Direct3D12: return "dx12";
        case bgfx::RendererType::Metal:      return "metal";
        default:                              return "spirv";
    }
}

bgfx::ShaderHandle load_shader_binary(const char* sc_filename)
{
    char path[512];
    std::snprintf(path, sizeof(path), SHADER_DIR "/%s/%s.bin", shader_backend(), sc_filename);

    FILE* f = std::fopen(path, "rb");
    if (!f)
    {
        std::fprintf(stderr, "load_shader_binary: cannot open %s\n", path);
        return BGFX_INVALID_HANDLE;
    }

    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    // Read into our own buffer first: a bgfx::alloc block is only released by
    // handing it to bgfx, so it can't be abandoned on a failed read.
    std::vector<uint8_t> bytes(static_cast<size_t>(size > 0 ? size : 0) + 1, 0);
    const size_t got = std::fread(bytes.data(), 1, bytes.size() - 1, f);
    std::fclose(f);
    if (got != bytes.size() - 1)
    {
        std::fprintf(stderr, "load_shader_binary: short read on %s\n", path);
        return BGFX_INVALID_HANDLE;
    }

    const bgfx::Memory* mem = bgfx::copy(bytes.data(), static_cast<uint32_t>(bytes.size()));
    return bgfx::createShader(mem);
}

} // namespace

bgfx::ProgramHandle load_program(const char* vs_name, const char* fs_name)
{
    const bgfx::ShaderHandle vs = load_shader_binary(vs_name);
    const bgfx::ShaderHandle fs = load_shader_binary(fs_name);
    if (!bgfx::isValid(vs) || !bgfx::isValid(fs))
    {
        if (bgfx::isValid(vs)) bgfx::destroy(vs);
        if (bgfx::isValid(fs)) bgfx::destroy(fs);
        return BGFX_INVALID_HANDLE;
    }
    return bgfx::createProgram(vs, fs, /*destroyShaders=*/true);
}

} // namespace render
