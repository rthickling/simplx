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
