# cmake/toolchain-clang.cmake
#
# Recommended toolchain for Eschaton.
#
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-clang.cmake -B build
#
# Why Clang:
#   - True cross-platform: Linux, macOS, Windows (MSVC-ABI or MinGW-ABI)
#   - Apple's Metal backend (required by bgfx on macOS/iOS) is built on Clang
#   - Strong standards compliance and superior diagnostics
#   - clang-format / clang-tidy integration out of the box
#   - LLVM IR enables LTO and polly optimizations for the hot physics path

set(CMAKE_C_COMPILER   clang)
set(CMAKE_CXX_COMPILER clang++)

# Use LLVM's archiver and ranlib so LTO works end-to-end.
find_program(LLVM_AR     NAMES llvm-ar     llvm-ar-18 llvm-ar-17 llvm-ar-16)
find_program(LLVM_RANLIB NAMES llvm-ranlib llvm-ranlib-18 llvm-ranlib-17 llvm-ranlib-16)

if(LLVM_AR)
    set(CMAKE_AR     "${LLVM_AR}"     CACHE FILEPATH "LLVM archiver")
endif()
if(LLVM_RANLIB)
    set(CMAKE_RANLIB "${LLVM_RANLIB}" CACHE FILEPATH "LLVM ranlib")
endif()

# Optional: enable Link-Time Optimization in Release builds.
# Uncomment when the project is more mature and compile times matter less.
# set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
