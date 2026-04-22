cmake_minimum_required(VERSION 3.16)
project(CloudSentry CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall -Wextra")

find_package(Threads REQUIRED)

# Main simulation
add_executable(cloudsentry main.cpp)
target_include_directories(cloudsentry PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(cloudsentry Threads::Threads)

# Skip list benchmark
add_executable(bench_skiplist benchmarks/skip_list_bench.cpp)
target_include_directories(bench_skiplist PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(bench_skiplist Threads::Threads)
# Shared library for Python API
add_library(cloudsentry_lib SHARED api/bridge.cpp)
target_include_directories(cloudsentry_lib PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(cloudsentry_lib Threads::Threads)
