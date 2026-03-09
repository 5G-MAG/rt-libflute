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

include(GoogleTest)

function(add_flute_test_executable target_name test_source test_prefix)
  add_executable(${target_name}
          ${test_source}
  )

  target_link_libraries(
          ${target_name}
          PRIVATE
          flute
          GTest::gtest_main
  )

  target_include_directories(${target_name} PRIVATE ${PROJECT_SOURCE_DIR}/include)

  gtest_discover_tests(${target_name}
          TEST_PREFIX ${test_prefix}
  )
endfunction()

add_flute_test_executable(flute_unit_tests test_transmitter.cpp "unit:")
add_flute_test_executable(flute_e2e_tests test_end_to_end.cpp "e2e:")
