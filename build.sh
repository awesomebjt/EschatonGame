#!/usr/bin/env bash
# Configure and build Eschaton.
#
# Usage: ./build.sh [debug|release] [--clean] [--run]
#   debug    (default) builds into build/
#   release  builds into build-release/
#   --clean  delete the build directory first (dependencies are re-fetched, which takes minutes)
#   --run    launch the game after a successful build
set -euo pipefail

cd "$(dirname "$0")"

build_type=Debug
build_dir=build
clean=0
run=0

for arg in "$@"; do
    case "$arg" in
        debug)   build_type=Debug;   build_dir=build ;;
        release) build_type=Release; build_dir=build-release ;;
        --clean) clean=1 ;;
        --run)   run=1 ;;
        -h|--help) sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $arg (try --help)" >&2; exit 1 ;;
    esac
done

if (( clean )); then
    rm -rf "$build_dir"
fi

cmake -S . -B "$build_dir" \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-clang.cmake \
    -DCMAKE_BUILD_TYPE="$build_type"

cmake --build "$build_dir" --target eschaton --parallel "$(nproc)"

echo "Built $build_dir/eschaton ($build_type)"

if (( run )); then
    exec "./$build_dir/eschaton"
fi
