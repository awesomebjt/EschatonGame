cmake_minimum_required(VERSION 3.25)

project(Eschaton
    VERSION 0.1.0
    DESCRIPTION "First-person 3D game set inside a pair of counter-rotating O'Neill cylinders"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Language standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)    # No GNU extensions — keep it portable

# ---------------------------------------------------------------------------
# Build-type default
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependencies via FetchContent
#
# Recommended compiler: Clang/LLVM
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-clang.cmake -B build
#
# All three libraries are fetched at configure time.  On a fresh clone
# this takes a few minutes; subsequent configures use the CMake cache.
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)   # Show download progress during first configure

# --- EnTT (header-only ECS) -------------------------------------------------
FetchContent_Declare(
    entt
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG        v3.13.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(entt)

# --- SDL3 -------------------------------------------------------------------
# Build as a static library so the binary is self-contained.
set(SDL_SHARED       OFF CACHE BOOL "" FORCE)
set(SDL_STATIC       ON  CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS        OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.2.8
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(SDL3)

# --- bgfx (via bgfx.cmake community CMake wrapper) --------------------------
# bgfx.cmake wraps bgfx + bx + bimg and exposes them as CMake targets.
# bgfx, bx, and bimg are included as git submodules inside the wrapper repo.
set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS    ON  CACHE BOOL "" FORCE)   # shaderc required for shader compilation
set(BGFX_INSTALL        OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    bgfx_cmake
    GIT_REPOSITORY        https://github.com/bkaradzic/bgfx.cmake.git
    GIT_TAG               master
    GIT_SHALLOW           FALSE   # bgfx.cmake uses submodules; shallow breaks them
    GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(bgfx_cmake)

# bgfxToolUtils.cmake is not auto-included via FetchContent; bring it in
# manually so bgfx_compile_shaders() and friends are available.
include(${bgfx_cmake_SOURCE_DIR}/cmake/bgfxToolUtils.cmake)

# Point shaderc at the bgfx shader common headers (bgfx_shader.sh, etc.)
set(BGFX_SHADER_INCLUDE_PATH "${bgfx_cmake_SOURCE_DIR}/bgfx/src"
    CACHE STRING "bgfx shader include directory" FORCE)

# ---------------------------------------------------------------------------
# Shader compilation
# ---------------------------------------------------------------------------
set(SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/shaders)

bgfx_compile_shaders(
    TYPE        VERTEX
    SHADERS     ${CMAKE_SOURCE_DIR}/shaders/vs_mesh.sc
    VARYING_DEF ${CMAKE_SOURCE_DIR}/shaders/varying.def.sc
    OUTPUT_DIR  ${SHADER_OUTPUT_DIR}
    OUT_FILES_VAR VS_OUTPUT_FILES
)

bgfx_compile_shaders(
    TYPE        FRAGMENT
    SHADERS     ${CMAKE_SOURCE_DIR}/shaders/fs_mesh.sc
    VARYING_DEF ${CMAKE_SOURCE_DIR}/shaders/varying.def.sc
    OUTPUT_DIR  ${SHADER_OUTPUT_DIR}
    OUT_FILES_VAR FS_OUTPUT_FILES
)

add_custom_target(compile_shaders ALL DEPENDS ${VS_OUTPUT_FILES} ${FS_OUTPUT_FILES})

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(eschaton
    src/main.cpp
)

add_dependencies(eschaton compile_shaders)

target_compile_definitions(eschaton PRIVATE
    SHADER_DIR="${SHADER_OUTPUT_DIR}"
)

target_compile_options(eschaton PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter   # EnTT internals trigger this
    >
    $<$<CXX_COMPILER_ID:MSVC>:
        /W4 /permissive-
    >
)

target_link_libraries(eschaton PRIVATE
    EnTT::EnTT
    SDL3::SDL3-static
    bgfx
    bx
    bimg
)

target_include_directories(eschaton PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# ---------------------------------------------------------------------------
# Install (optional, for packaging later)
# ---------------------------------------------------------------------------
install(TARGETS eschaton DESTINATION bin)
