# Building and debugging with just

Run commands from the repository root. `just` also finds the root justfile when
invoked from a subdirectory. The new root CMake project builds the engine once
and shares it between the tests and tutorials. It does not invoke the historical
per-tutorial CMakeLists.txt files or `test/build-and-test.sh`.

## Requirements

- Linux, GCC 16.2 with C++26 support (the supported compiler baseline).
- CMake 3.28 or newer, Ninja, Git and just. GDB is needed for `just debug`.
- The public Google Test submodule. Initialise it with `just deps`.

By default the justfile selects `g++-16`. If your compiler has another name:

```sh
export CXX=/opt/gcc-16/bin/g++
```

The new root CMake project also works with CMake 4.x; it does not inherit the
legacy policy requirements of the old standalone projects. GCC reflection is
enabled when the selected compiler accepts `-freflection`.

## Commands

```sh
just                         # List recipes
just deps                    # Initialise public Google Test only
just build                   # Debug: libraries, 15 test executables, 17 tutorials
just build-release           # Optimised Release build
just test                    # Build Debug, then run CTest
just test-release            # Build Release, then run CTest
just check                   # Build and test Debug and Release
just build relwithdebinfo    # Optimisation plus debug information
just target pingpong.bin     # Build only pingpong and its dependencies in Debug
just run pingpong.bin        # Build and run pingpong
just debug pingpong.bin      # Build pingpong in Debug, then launch GDB
```

`build-debug` and `test-debug` are explicit aliases for the default configuration.
Each build recipe configures first, so a clean checkout works after `just deps`.
Builds are incremental; requesting a build does not force recompilation of
unchanged sources. Tutorials are compiled but are not registered as tests:
some wait for keyboard input or run servers.

Tests run sequentially because they use CPU affinity and shared TCP ports. An
empty test selection or a failing executable makes `just test` fail. Tests have
a 300-second per-executable limit. Override the timeout or build parallelism with:

```sh
CTEST_TIMEOUT=600 CMAKE_BUILD_PARALLEL_LEVEL=4 just test
```

## Files and paths

| Item | Debug | Release |
| --- | --- | --- |
| CMake build directory | `build/debug` | `build/release` |
| Tutorial executable | `build/debug/bin/pingpong.bin` | `build/release/bin/pingpong.bin` |
| Compilation database | `build/debug/compile_commands.json` | `build/release/compile_commands.json` |

This is a separate build tree from existing per-tutorial `build/gcc16` directories.
The legacy standalone CMake files and their presets are unchanged.

For extra CMake configuration, configure the selected preset before invoking just;
the options persist in its CMake cache. For example:

```sh
CXX=/opt/gcc-16/bin/g++ cmake --preset debug \
    -DCMAKE_BUILD_RPATH=/opt/gcc-16/lib64
just test
```

Use a fresh build directory when changing compilers. To compile only tutorials
without Google Test, configure with `-DBUILD_TESTING=OFF`; `just test` will then
fail explicitly because no tests are registered.

## VS Code

Open the repository root, select the root `debug` configure preset in CMake Tools,
and select `pingpong.bin` as the launch target. CMake Tools can debug it directly.
In your existing launch.json the new fixed executable path is:

```json
"program": "${workspaceFolder}/build/debug/bin/pingpong.bin"
```

Alternatively, use `"program": "${command:cmake.launchTargetPath}"` to follow
the target selected in CMake Tools. Keep `"MIMode": "gdb"` and your existing
pretty-printing setup. Set breakpoints in both actors' `onEvent()` handlers.

## CI

The GitHub Actions workflow installs just and calls the same `just test debug`
and `just test release` recipes in its GCC 16.2 container. It no longer invokes
the old shell build script.
