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
