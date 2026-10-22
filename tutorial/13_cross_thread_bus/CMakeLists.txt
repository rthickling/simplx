# TCP sample 
cmake_minimum_required(VERSION 3.7.2)
set(TARGET_NAME tcp_sample.bin)

include("../../common_simplx.cmake")

trz_set_cxx_flags()

include_directories(${SIMPLX_DIR}/include)

trz_add_topdir(src/engine)

add_executable(main.v1.bin main.v1.cpp)
target_link_libraries(main.v1.bin PRIVATE engine)

add_executable(main.v2.bin main.v2.cpp)
target_link_libraries(main.v2.bin PRIVATE engine)

add_executable(main.v3.bin main.v3.cpp)
target_link_libraries(main.v3.bin PRIVATE engine)
