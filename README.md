# About

Simplx is a C++ development framework for building reliable cache-friendly distributed and concurrent multicore software.

Simplx was developed by [Tredzone SAS](http://www.tredzone.com). We provide software technology solutions and services dedicated to high-performance real-time processing. Our technology enables low and predictable latency, scalability, and high throughput. We also offer support contracts and enterprise tools for monitoring, profiling, debugging, server clustering under commercial licenses.

Simplx is used at the Paris stock exchange by Euronext's market exchange platform, called "Optiq", and has been successfully deployed since November 2016.

Tredzone was founded in 2013 and operates in France, the UK and US.


## Requirements

The current build configuration targets Linux with GCC 16.2, C++26 and the pthreads library.
It also enables GCC's reflection support. Earlier compiler versions listed in historical
documentation are no longer the supported build baseline.

Install Git, CMake 3.x (3.28 or newer recommended), and a build tool such as Make or Ninja.
Some project CMake files still use legacy policies, so CI currently uses CMake 3.x.


## License

Simplx is open-sourced under the Apache 2.0 license, please see the [accompanying License](./LICENSE).  


## Getting Started

About a dozen tutorials are included here, please see the [Tutorials README](./tutorial/README.md).

To build the first tutorial, open a terminal at the root of the repository and type:

```
cd tutorial/01_hello_actor
mkdir build && cd build
cmake ..
make -j8
```


## Build and test

From the repository root, with GCC 16.2 selected on your PATH:

```bash
git submodule update --init thirdparty/googletest
bash test/build-and-test.sh Debug
bash test/build-and-test.sh Release
```

The script builds and runs the engine and TCP test suites, then compiles every tracked
tutorial CMake project. Tutorials are built without running the interactive examples or
long-running servers. Failed builds, failed tests, empty test suites and tests exceeding
the timeout make the command fail. Tests run sequentially because they share CPU affinity
and network ports.

Google Test is pinned to the public v1.17.0 release. An older submodule reference was no
longer available from its configured upstream and prevented clean checkouts from building.

Ring-buffer tests directly exercise single-element, bulk and callback operations for both
fixed and dynamic capacities. These calls also make the compiler instantiate the template
members, so unused broken overloads cannot silently escape this coverage.

Builds go under `build/ci/<configuration>`. Set `CMAKE_BUILD_PARALLEL_LEVEL` to change the
default of two build workers, `CTEST_TIMEOUT` to change the per-executable timeout in seconds
(default 300), or `SIMPLX_BUILD_ROOT` to choose a different build directory.

Select compilers through `CC` and `CXX`; additional script arguments are passed to every
CMake configure invocation. For example:

```bash
CC=/opt/gcc-16/bin/gcc CXX=/opt/gcc-16/bin/g++ \
    bash test/build-and-test.sh Release -DCMAKE_BUILD_RPATH=/opt/gcc-16/lib64
```

Use a fresh `SIMPLX_BUILD_ROOT` when changing compilers or generators. The script does not
read personal `CMakeUserPresets.json` files, so local paths cannot affect CI.

[GitHub Actions](.github/workflows/build-and-test.yml) runs the same script in Debug and
Release on pull requests, pushes to `master`, and manual runs. Its official
`gcc:16.2.0-trixie` container supplies the compiler and runtime without relying on a local
installation. The workflow initializes only the public Google Test submodule.

The older `test/docker_test.sh` retains its historical compiler matrix; use the new script
and workflow for the GCC 16 build.
 

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


