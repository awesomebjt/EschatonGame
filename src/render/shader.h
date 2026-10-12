#pragma once

#include <string>

#include <bgfx/bgfx.h>

namespace render {

// Where compiled shaders live: <dir>/<backend>/<name>.bin. Defaults to the build
// tree (SHADER_DIR); a packaged game points it beside the executable.
void               set_shader_dir(std::string dir);
const std::string& shader_dir();

// Loads <shader_dir>/<backend>/<vs_name>.bin and <fs_name>.bin (the .sc source
// names, matching bgfx_compile_shaders output) and links them. Returns an
// invalid handle if either is missing.
bgfx::ProgramHandle load_program(const char* vs_name, const char* fs_name);

} // namespace render
