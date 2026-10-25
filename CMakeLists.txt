cmake_minimum_required(VERSION 3.28)
project(simplx LANGUAGES CXX)

# The root build is independent of the historical per-tutorial CMake projects.
if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 26)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# This project has no C++ modules; do not require clang-scan-deps.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# CMake 3.28 accepts the value 26 but may lack C++26 flag mappings for recent compilers.
# Check the compiler itself before configuring targets or FindThreads probes.
include(CheckCXXCompilerFlag)
if(CMAKE_CXX_STANDARD EQUAL 26 AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$"
   AND NOT DEFINED CMAKE_CXX26_STANDARD_COMPILE_OPTION)
    function(simplx_detect_cxx26)
        # The flag checks must use a dialect already understood by old CMake.
        set(CMAKE_CXX_STANDARD 23)
        check_cxx_compiler_flag("-std=c++26" SIMPLX_HAS_STD_CXX26)
        if(SIMPLX_HAS_STD_CXX26)
            set(CMAKE_CXX26_STANDARD_COMPILE_OPTION "-std=c++26" PARENT_SCOPE)
            set(CMAKE_CXX26_EXTENSION_COMPILE_OPTION "-std=gnu++26" PARENT_SCOPE)
        else()
            check_cxx_compiler_flag("-std=c++2c" SIMPLX_HAS_STD_CXX2C)
            if(NOT SIMPLX_HAS_STD_CXX2C)
                message(FATAL_ERROR "The selected compiler supports neither -std=c++26 nor -std=c++2c. Select a recent GCC or Clang compiler with CXX.")
            endif()
            set(CMAKE_CXX26_STANDARD_COMPILE_OPTION "-std=c++2c" PARENT_SCOPE)
            set(CMAKE_CXX26_EXTENSION_COMPILE_OPTION "-std=gnu++2c" PARENT_SCOPE)
        endif()
    endfunction()
    simplx_detect_cxx26()
    # Forward the mapping into nested try_compile projects (e.g. FindThreads).
    list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
        CMAKE_CXX26_STANDARD_COMPILE_OPTION
        CMAKE_CXX26_EXTENSION_COMPILE_OPTION)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(CTest)
include(CheckCXXCompilerFlag)
find_package(Threads REQUIRED)
option(SIMPLX_BUILD_TUTORIALS "Build the tutorial executables (do not run them)" ON)
option(SIMPLX_WARNINGS_AS_ERRORS "Treat warnings in simplx targets as errors" ON)

add_library(simplx_build_options INTERFACE)
target_compile_definitions(simplx_build_options INTERFACE TREDZONE_E2E=0)
target_link_libraries(simplx_build_options INTERFACE Threads::Threads)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(simplx_build_options INTERFACE
        -Wall -Wextra -Wpedantic -Wno-deprecated -Woverloaded-virtual
        -Wsign-promo -Wno-noexcept-type
        $<$<NOT:$<CONFIG:Debug>>:-Wno-unused-variable>
        $<$<BOOL:${SIMPLX_WARNINGS_AS_ERRORS}>:-Werror>)
    check_cxx_compiler_flag("-freflection" SIMPLX_HAS_REFLECTION)
    if(SIMPLX_HAS_REFLECTION)
        target_compile_options(simplx_build_options INTERFACE -freflection)
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Match the legacy deprecation policy without GCC-only warning switches.
    target_compile_options(simplx_build_options INTERFACE -Wno-deprecated
        $<$<BOOL:${SIMPLX_WARNINGS_AS_ERRORS}>:-Werror>)
endif()

add_library(engine STATIC
    src/engine/actor.cpp
    src/engine/engine.cpp
    src/engine/node.cpp
    src/engine/RefMapper.cpp
    src/engine/linux/platform_gcc.cpp
    src/engine/linux/platform_linux.cpp
    src/engine/parallel/parallel_xplat.cpp
    src/engine/e2e_stub.cpp)
target_include_directories(engine PUBLIC "${PROJECT_SOURCE_DIR}/include"
                                        "${PROJECT_SOURCE_DIR}")
target_link_libraries(engine PUBLIC simplx_build_options)

add_library(timer STATIC src/pattern/timer/timeractor.cpp)
target_link_libraries(timer PUBLIC engine)

if(BUILD_TESTING)
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/thirdparty/googletest/googletest/CMakeLists.txt")
        message(FATAL_ERROR "Google Test is missing. Run: just deps")
    endif()
    add_subdirectory(thirdparty/googletest EXCLUDE_FROM_ALL)

    function(simplx_test name source)
        add_executable(${name} ${source})
        target_link_libraries(${name} PRIVATE engine GTest::gtest_main ${ARGN})
        add_test(NAME ${name} COMMAND ${name})
        # These executables share affinity settings and TCP ports.
        set_tests_properties(${name} PROPERTIES RUN_SERIAL TRUE)
    endfunction()

    simplx_test(testasync.bin test/engine/testasync.cpp)
    simplx_test(testasyncactor.bin test/engine/testasyncactor.cpp)
    simplx_test(testasyncengine.bin test/engine/testasyncengine.cpp)
    simplx_test(testasynceventloop.bin test/engine/testasynceventloop.cpp)
    simplx_test(testasyncinitializer.bin test/engine/testasyncinitializer.cpp)
    simplx_test(testmdoublechain.bin test/engine/testmdoublechain.cpp)
    simplx_test(testmforwardchain.bin test/engine/testmforwardchain.cpp)
    simplx_test(testparallel.bin test/engine/testparallel.cpp)
    simplx_test(testproperty.bin test/engine/testproperty.cpp)
    simplx_test(testtime.bin test/engine/testtime.cpp)
    simplx_test(testtimer.bin test/engine/testtimeractor.cpp timer)
    simplx_test(teststream.bin test/engine/testdataiostream.cpp)
    simplx_test(testringbuffer.bin test/engine/testringbuffer.cpp)
    simplx_test(circularbuffertestu.bin test/connector/tcp/circularbuffertestu.cpp)
    simplx_test(clientservertestu.bin test/connector/tcp/clientserverbehaviortest.cpp)
    target_include_directories(clientservertestu.bin PRIVATE
        "${PROJECT_SOURCE_DIR}/test/connector/tcp")
endif()

if(SIMPLX_BUILD_TUTORIALS)
    function(simplx_tutorial name)
        add_executable(${name} ${ARGN})
        target_link_libraries(${name} PRIVATE engine)
    endfunction()

    simplx_tutorial(hello_actor.bin tutorial/01_hello_actor/hello_actor.cpp)
    simplx_tutorial(hello_world.bin tutorial/02_hello_world/hello_world.cpp)
    simplx_tutorial(printer_actor_starter.bin tutorial/03_printer_actor_starter/printer_actor_starter.cpp)
    simplx_tutorial(printer_actor_service.bin tutorial/04_printer_actor_service/printer_actor_service.cpp)
    simplx_tutorial(callback.bin tutorial/05_multi_callback/multi_callback.cpp)
    simplx_tutorial(undelivered_event_management.bin tutorial/06_undelivered_event_management/undelivered_event_management.cpp)
    simplx_tutorial(referenced_unreferenced_actor.bin tutorial/07_referenced_unreferenced_actor/ref_unref_actor.cpp)
    simplx_tutorial(pingpong.bin tutorial/08_pingpong/pingpong.cpp)
    simplx_tutorial(sync_exit.bin tutorial/09_sync_exit/sync_exit.cpp)
    simplx_tutorial(tuto_timer.bin tutorial/10_timer/timer.cpp)
    target_link_libraries(tuto_timer.bin PRIVATE timer)
    simplx_tutorial(keyboard.bin tutorial/11_keyboard_actor/keyboard.cpp
        src/pattern/keyboardactor.cpp src/pattern/waitcondition.cpp)
    simplx_tutorial(httpserver.bin tutorial/12_connector/http/sample/httpserver.cpp)
    simplx_tutorial(tcpclient.bin tutorial/12_connector/tcp/sample/tcpclient.cpp)
    simplx_tutorial(tcpserver.bin tutorial/12_connector/tcp/sample/tcpserver.cpp)
    simplx_tutorial(main.v1.bin tutorial/13_cross_thread_bus/main.v1.cpp)
    simplx_tutorial(main.v2.bin tutorial/13_cross_thread_bus/main.v2.cpp)
    simplx_tutorial(main.v3.bin tutorial/13_cross_thread_bus/main.v3.cpp)
endif()
