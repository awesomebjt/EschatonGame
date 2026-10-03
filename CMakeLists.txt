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

# --- mruby (embedded Ruby for the debug console) ----------------------------
# mruby builds with its own rake toolchain, not CMake, so fetch the source only
# and drive rake from a custom command. Requires ruby and rake on the host.
# mruby has no CMakeLists.txt, so MakeAvailable only downloads it.
FetchContent_Declare(
    mruby
    GIT_REPOSITORY https://github.com/mruby/mruby.git
    GIT_TAG        3.4.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(mruby)

find_program(RAKE_EXECUTABLE rake REQUIRED)
set(MRUBY_CONFIG    ${CMAKE_SOURCE_DIR}/cmake/mruby_config.rb)
set(MRUBY_BUILD_DIR ${CMAKE_BINARY_DIR}/mruby)
set(MRUBY_LIB       ${MRUBY_BUILD_DIR}/host/lib/libmruby.a)

add_custom_command(
    OUTPUT  ${MRUBY_LIB}
    COMMAND ${CMAKE_COMMAND} -E env
            MRUBY_CONFIG=${MRUBY_CONFIG}
            MRUBY_BUILD_DIR=${MRUBY_BUILD_DIR}
            ${RAKE_EXECUTABLE} -f ${mruby_SOURCE_DIR}/Rakefile all
    WORKING_DIRECTORY ${mruby_SOURCE_DIR}
    DEPENDS ${MRUBY_CONFIG}
    COMMENT "Building mruby"
    VERBATIM
)
add_custom_target(mruby_build DEPENDS ${MRUBY_LIB})

# The presym headers mruby generates live in the build tree, not the source.
file(MAKE_DIRECTORY ${MRUBY_BUILD_DIR}/host/include)
add_library(mruby STATIC IMPORTED)
set_target_properties(mruby PROPERTIES
    IMPORTED_LOCATION             ${MRUBY_LIB}
    INTERFACE_INCLUDE_DIRECTORIES "${mruby_SOURCE_DIR}/include;${MRUBY_BUILD_DIR}/host/include"
    INTERFACE_LINK_LIBRARIES      $<$<NOT:$<PLATFORM_ID:Windows>>:m>
)
add_dependencies(mruby mruby_build)

# ---------------------------------------------------------------------------
# Shader compilation
# ---------------------------------------------------------------------------
set(SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/shaders)
set(SHADER_SRC_DIR    ${CMAKE_SOURCE_DIR}/shaders)

bgfx_compile_shaders(
    TYPE        VERTEX
    SHADERS     ${SHADER_SRC_DIR}/vs_terrain.sc
                ${SHADER_SRC_DIR}/vs_building.sc
                ${SHADER_SRC_DIR}/vs_overlay.sc
                ${SHADER_SRC_DIR}/vs_cloud.sc
    VARYING_DEF ${SHADER_SRC_DIR}/varying.def.sc
    INCLUDE_DIRS ${SHADER_SRC_DIR}
    OUTPUT_DIR  ${SHADER_OUTPUT_DIR}
    OUT_FILES_VAR VS_OUTPUT_FILES
)

bgfx_compile_shaders(
    TYPE        FRAGMENT
    SHADERS     ${SHADER_SRC_DIR}/fs_world.sc
                ${SHADER_SRC_DIR}/fs_overlay.sc
                ${SHADER_SRC_DIR}/fs_cloud.sc
    VARYING_DEF ${SHADER_SRC_DIR}/varying.def.sc
    INCLUDE_DIRS ${SHADER_SRC_DIR}
    OUTPUT_DIR  ${SHADER_OUTPUT_DIR}
    OUT_FILES_VAR FS_OUTPUT_FILES
)

# bgfx_compile_shaders only tracks the .sc and varying.def; make the shared
# include rebuild its users too.
foreach(out ${VS_OUTPUT_FILES})
    add_custom_command(OUTPUT ${out} APPEND DEPENDS ${SHADER_SRC_DIR}/cylinder.sh)
endforeach()
foreach(out ${FS_OUTPUT_FILES})
    add_custom_command(OUTPUT ${out} APPEND DEPENDS ${SHADER_SRC_DIR}/fog.sh)
endforeach()

add_custom_target(compile_shaders ALL DEPENDS ${VS_OUTPUT_FILES} ${FS_OUTPUT_FILES})

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(eschaton
    src/main.cpp
    src/core/json.cpp
    src/debug/bindings.cpp
    src/debug/console.cpp
    src/game/player.cpp
    src/physics/character.cpp
    src/physics/collision_world.cpp
    src/render/cloud_renderer.cpp
    src/render/shader.cpp
    src/render/world_renderer.cpp
    src/world/clouds.cpp
    src/world/formats.cpp
    src/world/world_loader.cpp
)

add_dependencies(eschaton compile_shaders)

target_compile_definitions(eschaton PRIVATE
    SHADER_DIR="${SHADER_OUTPUT_DIR}"
    WORLD_DIR="${CMAKE_SOURCE_DIR}/world"
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

find_package(Threads REQUIRED)

target_link_libraries(eschaton PRIVATE
    Threads::Threads
    EnTT::EnTT
    SDL3::SDL3-static
    bgfx
    bx
    bimg
    mruby
)

target_include_directories(eschaton PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# ---------------------------------------------------------------------------
# Headless physics checks against the exported world (no window or GPU)
# ---------------------------------------------------------------------------
add_executable(physics_check EXCLUDE_FROM_ALL
    tests/physics_check.cpp
    src/core/json.cpp
    src/game/player.cpp
    src/physics/character.cpp
    src/physics/collision_world.cpp
    src/world/clouds.cpp
    src/world/formats.cpp
    src/world/world_loader.cpp
)
target_compile_definitions(physics_check PRIVATE WORLD_DIR="${CMAKE_SOURCE_DIR}/world")
target_include_directories(physics_check PRIVATE ${CMAKE_SOURCE_DIR}/src)
# bgfx only for headers (CameraPos lives beside the renderer).
target_link_libraries(physics_check PRIVATE Threads::Threads EnTT::EnTT bgfx)

add_executable(clouds_check EXCLUDE_FROM_ALL tests/clouds_check.cpp src/world/clouds.cpp)
target_include_directories(clouds_check PRIVATE ${CMAKE_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# Install (optional, for packaging later)
# ---------------------------------------------------------------------------
install(TARGETS eschaton DESTINATION bin)
