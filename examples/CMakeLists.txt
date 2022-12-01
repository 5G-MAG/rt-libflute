cmake_minimum_required(VERSION 3.16)

include(FindPkgConfig)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(CMAKE_CXX_FLAGS_DEBUG_INIT "-Wall -Wextra -Werror -g3")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-Wall -O3")

find_package(Boost REQUIRED)
find_package(spdlog REQUIRED)
find_package(PkgConfig REQUIRED)

include_directories(
    "${PROJECT_BINARY_DIR}"
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src

    SYSTEM
    ${SPDLOG_INCLUDEDIR}
    )

set(CMAKE_CXX_CLANG_TIDY clang-tidy --format-style=google --checks=clang-diagnostic-*,clang-analyzer-*,-*,bugprone*,modernize*,performance*)

add_executable(flute-transmitter flute-transmitter.cpp)
add_executable(flute-receiver flute-receiver.cpp)

target_link_libraries( flute-transmitter
    LINK_PUBLIC
    spdlog::spdlog
    config++
    flute
    pthread
    m
)
target_link_libraries( flute-receiver
    LINK_PUBLIC
    spdlog::spdlog
    config++
    flute
    pthread
    m
)
