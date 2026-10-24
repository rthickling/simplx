# About

Simplx is a C++ development framework for building reliable cache-friendly distributed and concurrent multicore software.

Simplx was developed by [Tredzone SAS](http://www.tredzone.com). We provide software technology solutions and services dedicated to high-performance real-time processing. Our technology enables low and predictable latency, scalability, and high throughput. We also offer support contracts and enterprise tools for monitoring, profiling, debugging, server clustering under commercial licenses.

Simplx is used at the Paris stock exchange by Euronext's market exchange platform, called "Optiq", and has been successfully deployed since November 2016.

Tredzone was founded in 2013 and operates in France, the UK and US.


## Requirements

The current build configuration targets Linux with GCC 16.2, C++26 and the pthreads library.
It also enables GCC's reflection support. Earlier compiler versions listed in historical
documentation are no longer the supported build baseline.

Install Git, CMake 3.28 or newer, Ninja and just. The new root build supports CMake 4.x
and builds the engine once for all tutorials and tests. See [BUILDING.md](BUILDING.md).


## License

Simplx is open-sourced under the Apache 2.0 license, please see the [accompanying License](./LICENSE).  


## Getting Started

About a dozen tutorials are included here, please see the [Tutorials README](./tutorial/README.md).

To build and run the first tutorial, open a terminal at the repository root:

```sh
just deps
just run hello_actor.bin
```


## Build and test

Use the root justfile with GCC 16.2 selected (the default executable name is `g++-16`):

```sh
just deps
just build-debug
just test-debug
just test-release
just check
```

`just check` builds and tests Debug and Release. Builds include all tutorials,
without running interactive examples or long-running servers. The 15 registered
CTest executables run sequentially because they share CPU affinity and TCP ports.
A failed build, failed test, timeout or empty test selection fails the command.

Binaries are under `build/debug/bin` and `build/release/bin`. For pingpong:

```sh
just target pingpong.bin
just debug pingpong.bin
```

Use `CXX=/path/to/g++ just test` to select your compiler. Set
`CMAKE_BUILD_PARALLEL_LEVEL` for build parallelism (default 2), or `CTEST_TIMEOUT`
for the per-test timeout (default 300 seconds). Use a fresh build tree when
changing compilers. Detailed commands, CMake options and VS Code setup are in
[BUILDING.md](BUILDING.md).

Google Test remains pinned to v1.17.0. `just deps` initializes only this public
submodule. [GitHub Actions](.github/workflows/build-and-test.yml) now runs the same
just recipes in its GCC 16.2 container, replacing the old build script.

## Documentation

To generate the documentation, open a terminal at the repository root and type

```
doxygen doc/Doxyfile
```


## Enterprise License

If you have a commercial enterprise license with us, including appropriate github access credentials, you may fetch those components with:

```
git submodule update --init enterprise
```

for compilation you should also #define the following:

```
TREDZONE_ENTREPRISE
```

## Enterprise Unit Tests

At the root of the repository and type:

```
git submodule update --init thirdparty/googletest
mkdir tbuild && cd tbuild
cmake -DENTERPRISE=1 -DBUILD_TEST=1 ..
make -j8
make test
```


## Docker

To run the unit tests under Docker:

```
./test/docker_test.sh -DENTERPRISE=1
```


