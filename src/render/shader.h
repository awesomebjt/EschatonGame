#pragma once

#include <bgfx/bgfx.h>

namespace render {

// Loads SHADER_DIR/<backend>/<vs_name>.bin and <fs_name>.bin (the .sc source
// names, matching bgfx_compile_shaders output) and links them. Returns an
// invalid handle if either is missing.
bgfx::ProgramHandle load_program(const char* vs_name, const char* fs_name);

} // namespace render
