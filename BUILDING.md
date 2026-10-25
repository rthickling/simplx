# Building and debugging with just

Run commands from the repository root. `just` also finds the root justfile when
invoked from a subdirectory. The new root CMake project builds the engine once
and shares it between the tests and tutorials. It does not invoke the historical
per-tutorial CMakeLists.txt files or `test/build-and-test.sh`.

## Requirements

- Linux, GCC 16.2 or Clang 22 with C++26 mode support.
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

The GitHub Actions workflow runs four jobs: GCC 16.2 and Clang 22, each in Debug
and Release. Each job installs the build tools and calls the same `just test`
recipe used locally. A failure in any job fails the workflow.


## Docker compiler matrix

`test/docker_test.sh` has been replaced by these root justfile recipes:

```sh
just docker-check                 # GCC + Clang, Debug + Release
just docker-test gcc debug         # One compiler/configuration
just docker-test clang release
just docker-test clang relwithdebinfo
just docker-build clang            # Build/refresh only the tool image
```

The host needs Docker with a working daemon, Git and just; CMake, Ninja and the
compilers run inside containers. `docker-test` initialises the public Google
Test submodule on the host. It builds the engine, all 17 tutorials and all 15
test executables, then runs the tests sequentially. Tutorials are build-only.
A failed image build, compilation or test returns a nonzero status. There is no
interactive terminal requirement, log-marker parsing or privileged container.

The default base images are `gcc:16.2.0-trixie` and `silkeh/clang:22-trixie`.
They use versioned tags rather than `latest`; Clang uses the image's default
Linux standard library (libstdc++), not a separate libc++ matrix. These are C++26
language-mode builds, not a claim of complete C++26 feature support. GCC's
`-freflection` is probed separately; it is not passed to Clang. GCC retains its
explicit warning set; Clang treats its default enabled warnings as errors; both retain the legacy
deprecation-warning suppression.

`test/Dockerfile` adds the common build tools; `test/.dockerignore` excludes
sources and historical build trees from the image context. Docker caches that layer between
runs; `--pull` checks for updates to the selected base image. Override either
image explicitly when trying another version, for example:

```sh
SIMPLX_CLANG_IMAGE=silkeh/clang:21-trixie just docker-test clang debug
```

When changing the image/compiler version, first remove or move that compiler's
`build/docker/gcc` or `build/docker/clang` directory to avoid reusing its CMake
cache. `docker-check` runs sequentially and stops on the first failure; GitHub
Actions runs the four jobs independently, so every result is visible.

Sources are mounted read-only; only the selected Docker build directory is
writable. Containers run with your host UID/GID, so output files belong to you.
Builds persist at `build/docker/<compiler>/<configuration>` (for example,
`build/docker/clang/debug/bin/pingpong.bin`), separate from native
`build/debug` and `build/release`. These binaries use container toolchain runtime
libraries and are intended to run inside that container. GitHub Actions uses
fresh workspaces, so its existing preset paths need no additional isolation.

Compiler matrix changes should update both the justfile's image defaults and
the GitHub Actions matrix. Test timeouts and parallelism use the same
`CTEST_TIMEOUT` and `CMAKE_BUILD_PARALLEL_LEVEL` environment overrides as native
builds. Docker correctness tests are separate from latency measurements on
pinned cores.

Clang's diagnostics also exposed dangling `what()` message pointers in five TCP
exception classes. Those exceptions now own their message strings and copy
without retaining references to constructor arguments. The TCP test executable
includes message-lifetime and copy regression checks.
