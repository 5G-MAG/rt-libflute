cmake_minimum_required(VERSION 3.16)

project (libflute VERSION 0.11.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(CMAKE_CXX_FLAGS_DEBUG_INIT "-Wall -Wextra -Werror -g3")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-Wall -O3")

find_package(Boost REQUIRED)
find_package(spdlog REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
pkg_check_modules(TINYXML REQUIRED IMPORTED_TARGET tinyxml2)
pkg_check_modules(NETLINK REQUIRED IMPORTED_TARGET libnl-3.0)

add_subdirectory(examples)

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
target_sources(flute
  PRIVATE
    src/Receiver.cpp src/Transmitter.cpp src/AlcPacket.cpp src/File.cpp src/EncodingSymbol.cpp src/FileDeliveryTable.cpp src/IpSec.cpp src/RaptorFEC.cpp src/gf2matrix.cpp src/raptor10.cpp src/raptor_consts.cpp
    utils/base64.cpp 
  PUBLIC
    include/Receiver.h include/Transmitter.h include/File.h include/RaptorFEC.h include/gf2matrix.h include/raptor10.h include/raptor_consts.h
  )
target_include_directories(flute
  PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/include/
  )

#add_library(flute src/Receiver.cpp src/Receiver.h src/AlcPacket.cpp src/File.cpp src/EncodingSymbol.cpp src/FileDeliveryTable.cpp freeRaptor/gf2matrix.c freeRaptor/raptor10.c freeRaptor/raptor_consts.c)

target_link_libraries( flute
    LINK_PUBLIC
    spdlog::spdlog
    z
    pthread
    crypto
    PkgConfig::TINYXML
    PkgConfig::NETLINK
    m
)

