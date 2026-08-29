cmake_minimum_required(VERSION 3.25)
project(kernsql LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # for clangd

add_library(kernsql_lib STATIC
  src/common/logger.cpp
  src/storage/disk_manager.cpp
  src/buffer/buffer_pool_manager.cpp
  src/buffer/page_guard.cpp)
target_include_directories(kernsql_lib PUBLIC src)
target_compile_options(kernsql_lib PUBLIC -Wall -Wextra -Wpedantic -Wshadow -Wconversion
  -Werror=format)

add_executable(kernsql src/shell/main.cpp)
target_link_libraries(kernsql PRIVATE kernsql_lib)

include(FetchContent)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2)
FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)
file(GLOB_RECURSE TEST_SRCS CONFIGURE_DEPENDS test/*_test.cpp)
foreach(t ${TEST_SRCS})
  get_filename_component(name ${t} NAME_WE)
  add_executable(${name} ${t})
  target_link_libraries(${name} PRIVATE kernsql_lib GTest::gtest_main)
  # A dropped condvar notify makes a concurrency test HANG rather than fail, and a hung test
  # blocks the run forever instead of reporting. Generous because TSan is roughly 10x slower.
  gtest_discover_tests(${name} PROPERTIES TIMEOUT 120)
endforeach()
