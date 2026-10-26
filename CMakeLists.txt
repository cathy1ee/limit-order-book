cmake_minimum_required(VERSION 3.20)
project(limit_order_book LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(lob src/order_book.cpp)
target_include_directories(lob PUBLIC include)
target_compile_options(lob PRIVATE -Wall -Wextra -Wpedantic)

add_executable(demo examples/demo.cpp)
target_link_libraries(demo PRIVATE lob)

# ---- Tests ----
option(LOB_BUILD_TESTS "Build unit tests" ON)
if(LOB_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(lob_tests tests/test_order_book.cpp)
  target_link_libraries(lob_tests PRIVATE lob GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(lob_tests)
endif()
