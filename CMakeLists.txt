cmake_minimum_required(VERSION 3.20)
project(orderbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(googletest)

# Warnings are PUBLIC so they reach our own code and tests (which link this target),
# but not GoogleTest's sources, which never link it.
add_library(orderbook STATIC src/order_book.cpp)
target_include_directories(orderbook PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_options(orderbook PUBLIC -Wall -Wextra -Werror)

enable_testing()
add_executable(orderbook_tests tests/test_matching.cpp)
target_link_libraries(orderbook_tests PRIVATE orderbook GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(orderbook_tests)
