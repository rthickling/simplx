# Run from the repository root, or any directory below it.
# Default compiler matches this fork. Override with CXX=/path/to/g++.
export CXX := env_var_or_default("CXX", "g++-16")
export CMAKE_BUILD_PARALLEL_LEVEL := env_var_or_default("CMAKE_BUILD_PARALLEL_LEVEL", "2")
export CTEST_TIMEOUT := env_var_or_default("CTEST_TIMEOUT", "300")

# Show available commands.
default:
    @just --list

# Initialise only the public Google Test submodule.
deps:
    git submodule update --init --depth 1 thirdparty/googletest

# Configure a root build: debug, release, or relwithdebinfo.
configure mode="debug":
    cmake --preset "{{ mode }}"

# Configure and build the engine, tests and all tutorials.
build mode="debug": (configure mode)
    cmake --build --preset "{{ mode }}" --parallel

# Build first, then run the registered tests sequentially.
test mode="debug": (build mode)
    ctest --preset "{{ mode }}" --timeout "$CTEST_TIMEOUT"

# Build and test both configurations.
check: (test "debug") (test "release")

# Build Debug binaries with symbols and without optimisation.
build-debug: (build "debug")

# Build optimised Release binaries.
build-release: (build "release")

# Build and test Debug.
test-debug: (test "debug")

# Build and test Release.
test-release: (test "release")

# Build one named CMake target, e.g. just target pingpong.bin debug.
target name mode="debug": (configure mode)
    cmake --build --preset "{{ mode }}" --target "{{ name }}" --parallel

# Build and run a tutorial, e.g. just run pingpong.bin debug.
run name="pingpong.bin" mode="debug": (target name mode)
    "./build/{{ mode }}/bin/{{ name }}"

# Build a Debug tutorial and launch GDB.
debug name="pingpong.bin": (target name "debug")
    gdb --args "./build/debug/bin/{{ name }}"

# Build a cached test image: gcc (16.2) or clang (22).
[positional-arguments]
docker-build compiler="gcc":
    #!/usr/bin/env bash
    set -euo pipefail
    case "$1" in
        gcc) base="${SIMPLX_GCC_IMAGE:-gcc:16.2.0-trixie}" ;;
        clang) base="${SIMPLX_CLANG_IMAGE:-silkeh/clang:22-trixie}" ;;
        *) echo "Unknown compiler: $1 (expected gcc or clang)" >&2; exit 2 ;;
    esac
    docker build --pull --build-arg "COMPILER_IMAGE=$base" \
        --tag "simplx-test:$1" --file test/Dockerfile test

# Build and test one compiler/configuration; host needs only Docker, Git and just.
[positional-arguments]
docker-test compiler="gcc" mode="debug": (docker-build compiler) deps
    #!/usr/bin/env bash
    set -euo pipefail
    case "$1" in
        gcc) cc=gcc; cxx=g++ ;;
        clang) cc=clang; cxx=clang++ ;;
        *) echo "Unknown compiler: $1 (expected gcc or clang)" >&2; exit 2 ;;
    esac
    case "$2" in
        debug|release|relwithdebinfo) ;;
        *) echo "Unknown configuration: $2" >&2; exit 2 ;;
    esac
    mkdir -p "build/docker/$1"
    docker run --rm --user "$(id -u):$(id -g)" \
        --mount "type=bind,src=$PWD,dst=/simplx,readonly" \
        --mount "type=bind,src=$PWD/build/docker/$1,dst=/simplx/build" \
        --workdir /simplx --env "CC=$cc" --env "CXX=$cxx" \
        --env CMAKE_BUILD_PARALLEL_LEVEL --env CTEST_TIMEOUT \
        "simplx-test:$1" bash -euc '"$CXX" --version; cmake --version; just test "$1"' bash "$2"

# Run GCC and Clang, each in Debug and Release (stop on the first failure).
docker-check: (docker-test "gcc" "debug") (docker-test "gcc" "release") (docker-test "clang" "debug") (docker-test "clang" "release")
