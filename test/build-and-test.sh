#!/usr/bin/env bash
# Build the existing independent CMake projects, then run their registered tests.
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_type="${1:-Release}"
if (( $# > 0 )); then
    shift
fi
case "$build_type" in
    Debug|Release|RelWithDebInfo|MinSizeRel) ;;
    *)
        echo "Usage: bash test/build-and-test.sh [build-type] [CMake options ...]" >&2
        exit 2
        ;;
esac

for tool in cmake ctest git; do
    command -v "$tool" >/dev/null || { echo "Required tool not found: $tool" >&2; exit 1; }
done
if [[ ! -f "$repo_root/thirdparty/googletest/googletest/CMakeLists.txt" ]]; then
    echo "Run: git submodule update --init thirdparty/googletest" >&2
    exit 1
fi

build_root="${SIMPLX_BUILD_ROOT:-$repo_root/build/ci}"
if [[ "$build_root" != /* ]]; then
    build_root="$repo_root/$build_root"
fi
build_root="$build_root/$build_type"

build_project()
{
    local project="$1"
    shift
    local build_dir="$build_root/$project"
    printf '\nBuilding %s (%s)\n' "$project" "$build_type"
    cmake -S "$repo_root/$project" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE="$build_type" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "$@"
    cmake --build "$build_dir" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
}

for project in test/engine test/connector/tcp; do
    build_project "$project" "$@"
    # The tests share CPU affinity and TCP ports; run executables sequentially.
    ctest --test-dir "$build_root/$project" --output-on-failure \
        --no-tests=error --parallel 1 --timeout "${CTEST_TIMEOUT:-300}"
done

# Use tracked paths so generated CMake files in local build trees are excluded.
tutorial_list="$(git -C "$repo_root" ls-files -- 'tutorial/**/CMakeLists.txt')"
if [[ -z "$tutorial_list" ]]; then
    echo "No tutorial CMake projects found in the Git checkout" >&2
    exit 1
fi
while IFS= read -r cmake_file; do
    build_project "${cmake_file%/CMakeLists.txt}" "$@"
done <<< "$tutorial_list"

echo "All test suites passed and all tutorials built ($build_type)."
