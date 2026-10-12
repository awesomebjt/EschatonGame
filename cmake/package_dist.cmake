# Assembles a self-contained copy of the game (cmake --build build --target dist):
#
#   <DIST_DIR>/eschaton             the executable
#   <DIST_DIR>/shaders/<backend>/   compiled shaders
#   <DIST_DIR>/world/               manifest, prototypes and chunks: what load_world reads
#
# plus <DIST_DIR>.tar.gz beside it (.zip for a Windows build). The executable
# finds world/ and shaders/ next to itself (resolve_data_dirs in main.cpp). Run
# with cmake -P; globbing here rather than at configure time picks up a
# re-export without re-running CMake.

foreach(var EXE SHADER_DIR WORLD_DIR DIST_DIR)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "package_dist.cmake: ${var} not set")
    endif()
endforeach()

if(NOT EXISTS "${WORLD_DIR}/manifest.json")
    message(FATAL_ERROR "package_dist.cmake: no manifest.json in ${WORLD_DIR}")
endif()

# Start clean so files dropped from the world don't linger in the package.
file(REMOVE_RECURSE "${DIST_DIR}")
file(MAKE_DIRECTORY "${DIST_DIR}/world")

file(COPY "${EXE}" DESTINATION "${DIST_DIR}")
file(COPY "${SHADER_DIR}/" DESTINATION "${DIST_DIR}/shaders")

# Only what the game loads; the .blend, height map and map tiles stay behind.
file(GLOB world_files
     "${WORLD_DIR}/manifest.json"
     "${WORLD_DIR}/prototypes.json"
     "${WORLD_DIR}/prototypes*.bin"
     "${WORLD_DIR}/chunk_*.bin")
file(COPY ${world_files} DESTINATION "${DIST_DIR}/world")

list(LENGTH world_files n_world)
get_filename_component(dist_parent "${DIST_DIR}" DIRECTORY)
get_filename_component(dist_name "${DIST_DIR}" NAME)
# A Windows build gets a zip, which Explorer opens without extra tools.
if(EXE MATCHES "\\.exe$")
    set(archive "${dist_name}.zip")
    set(tar_args cf "${archive}" --format=zip)
else()
    set(archive "${dist_name}.tar.gz")
    set(tar_args czf "${archive}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar ${tar_args} "${dist_name}"
    WORKING_DIRECTORY "${dist_parent}"
    RESULT_VARIABLE tar_result)
if(NOT tar_result EQUAL 0)
    message(FATAL_ERROR "package_dist.cmake: tar failed")
endif()

message(STATUS "dist: ${DIST_DIR} (${n_world} world files) and ${dist_parent}/${archive}")
