cmake_minimum_required(VERSION 3.16)
project(perfsim CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra)
# Profiling the simulator is one of the project's goals, so keep symbols and
# frame pointers even in optimised builds.
add_compile_options(-g -fno-omit-frame-pointer)

add_library(perfsim_core STATIC
  src/cache.cpp
  src/config.cpp
  src/cpu.cpp
  src/json.cpp
  src/memory.cpp
  src/report.cpp
  src/simulator.cpp
  src/trace.cpp
)
target_include_directories(perfsim_core PUBLIC include)

add_executable(perfsim src/main.cpp)
target_link_libraries(perfsim PRIVATE perfsim_core)

add_executable(tracegen
  workloads/main.cpp
  workloads/matrix.cpp
  workloads/pointer_chase.cpp
  workloads/random_access.cpp
  workloads/sequential.cpp
  workloads/strided.cpp
)
target_link_libraries(tracegen PRIVATE perfsim_core)
target_include_directories(tracegen PRIVATE workloads)

add_executable(perfcount tools/perfcount.cpp)
target_link_libraries(perfcount PRIVATE perfsim_core)

enable_testing()
add_executable(perfsim_tests
  tests/main.cpp
  tests/test_cache.cpp
  tests/test_config.cpp
  tests/test_cpu.cpp
  tests/test_json.cpp
  tests/test_trace.cpp
)
target_link_libraries(perfsim_tests PRIVATE perfsim_core)
target_include_directories(perfsim_tests PRIVATE tests)
add_test(NAME perfsim_tests COMMAND perfsim_tests)
