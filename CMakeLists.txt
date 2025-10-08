cmake_minimum_required(VERSION 3.16)

project (libflute VERSION 0.11.0)

include(CheckCXXSymbolExists)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(CMAKE_CXX_FLAGS_DEBUG_INIT "-Wall -Wextra -Werror -g3")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-Wall -O3")

find_package(Boost CONFIG REQUIRED)
find_package(spdlog REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
pkg_check_modules(TINYXML REQUIRED IMPORTED_TARGET tinyxml2)
pkg_check_modules(NETLINK REQUIRED IMPORTED_TARGET libnl-3.0)
pkg_check_modules(LIBCONFIG REQUIRED IMPORTED_TARGET libconfig++)
pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)
check_cxx_symbol_exists(mmap "sys/mman.h" HAVE_MMAP)

# Option to build example programs
option(BUILD_EXAMPLES "Build example transmitter/receiver programs" ON)
if (BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

include_directories(
    "${PROJECT_BINARY_DIR}"
    ${PROJECT_SOURCE_DIR}/include

    SYSTEM
    ${PROJECT_SOURCE_DIR}/utils
    ${SPDLOG_INCLUDEDIR}
    PkgConfig::NETLINK
    )

configure_file("include/Version.h.in" "Version.h")

link_directories(
    )

set(CMAKE_CXX_CLANG_TIDY clang-tidy)

add_library(flute "")
if (HAVE_MMAP)
    target_compile_definitions(flute PRIVATE HAVE_MMAP=1)
endif()

target_sources(flute
  PRIVATE
  src/Receiver.cpp src/Transmitter.cpp src/AlcPacket.cpp src/File.cpp src/EncodingSymbol.cpp src/FileDeliveryTable.cpp src/IpSec.cpp
    utils/base64.cpp
  PUBLIC
    include/Receiver.h include/Transmitter.h include/File.h
  )
target_include_directories(flute
  PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/include/
  )

target_link_libraries( flute
    LINK_PUBLIC
    spdlog::spdlog
    z
    pthread
    crypto
    PkgConfig::TINYXML
    PkgConfig::NETLINK
    PkgConfig::ZLIB
)

# ---- Tests Subdirectory (optional) ----
option(BUILD_TESTING "Build unit tests" ON)
if (BUILD_TESTING AND NOT DEFINED GTEST_DISABLE)
  add_subdirectory(tests)
endif()
