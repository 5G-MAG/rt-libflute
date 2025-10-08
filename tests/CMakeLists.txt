# CMake configuration for unit tests
# Assumes parent CMakeLists.txt set option(BUILD_TESTING ...)
cmake_minimum_required(VERSION 3.16)

# GoogleTest requires at least C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Fetch GoogleTest only if not already provided and not explicitly disabled

FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
        GIT_SHALLOW TRUE
)

# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(flute_tests
        test_transmitter.cpp
)

# Link against library under test
target_link_libraries(
        flute_tests
        PRIVATE
        flute
        GTest::gtest_main
)

target_include_directories(flute_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
include(GoogleTest)
gtest_discover_tests(flute_tests)
