# Build the standalone Linux CPU isolation tests.
isolation-build:
    cmake -S . -B build/isolation -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DSIMPLX_BUILD_TUTORIALS=OFF -DSIMPLX_BUILD_ISOLATION_TESTS=ON
    cmake --build build/isolation --target simplx_isolation_tests --parallel

# Build and run the CPU isolation suite. Pass launcher options after --.
[positional-arguments]
test-isolation *options: isolation-build
    #!/usr/bin/env bash
    if [[ ${1-} == -- ]]; then shift; fi
    exec python3 tools/isolation.py run --build build/isolation "$@"
