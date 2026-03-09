# AGENTS.md

Guidelines for AI coding agents working in the rt-libflute repository.

## Project Overview

C++17 library implementing the FLUTE/ALC protocol (File Delivery over Unidirectional Transport).
Maintained by 5G-MAG. Build system: CMake (3.16+) with Ninja generator.
Library target name: `flute`. Linux-only (requires `libnl-3.0`).

## Build Commands

```bash
# Configure and build (Release, with tests)
cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel

# Debug build
cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel

# Build without tests
cmake -S . -B build -GNinja -DBUILD_TESTING=OFF

# Build without examples
cmake -S . -B build -GNinja -DBUILD_EXAMPLES=OFF
```

Compiler flags are set in the root `CMakeLists.txt`:
- Debug: `-Wall -Wextra -Werror -g3`
- Release: `-Wall -O3`

## Test Commands

Test framework: Google Test v1.17.0 (fetched automatically via CMake FetchContent).
Test source: `tests/test_transmitter.cpp`. Single binary: `flute_tests`.

```bash
# Run all tests
ctest --test-dir build/tests --output-on-failure -j 2

# List available tests without running
ctest --test-dir build/tests -N

# Run a single test by name (regex match)
ctest --test-dir build/tests -R "RateLimitGetterSetter"

# Run a single test via GoogleTest binary directly
./build/tests/flute_tests --gtest_filter="TransmitterGetterSetterTest.RateLimitGetterSetter"

# Run all tests in a suite
./build/tests/flute_tests --gtest_filter="TransmitterGetterSetterTest.*"

# List all tests from the binary
./build/tests/flute_tests --gtest_list_tests
```

## Lint / Static Analysis

clang-tidy runs automatically during compilation (set globally in `CMakeLists.txt:44`).
Configuration lives in `.clang-tidy`:
- **Checks enabled:** `clang-diagnostic-*`, `clang-analyzer-*`, `bugprone*`, `modernize*`, `performance*`
- **Checks disabled:** `bugprone-narrowing-conversions`, `bugprone-implicit-widening-of-multiplication-result`,
  `bugprone-easily-swappable-parameters`, `modernize-use-trailing-return-type`, `modernize-avoid-c-arrays`,
  `modernize-avoid-bind`
- **Format style:** Google

There is no `.clang-format` file. No separate lint command is needed; linting is part of the build.

## System Dependencies

Install before building (Ubuntu/Debian):
```bash
sudo apt-get install -y ninja-build build-essential cmake \
  libboost-all-dev libspdlog-dev libtinyxml2-dev libconfig++-dev \
  libssl-dev libnl-3-dev zlib1g-dev ccache clang-tidy clang g++-12
```

## Project Structure

```
include/          Public API headers (Receiver.h, Transmitter.h, File.h, AlcPacket.h, etc.)
src/              Implementation files (.cpp)
utils/            Third-party utilities (base64)
examples/         Demo apps (flute-receiver, flute-transmitter)
tests/            Unit tests (Google Test)
```

## Code Style Guidelines

### Naming Conventions

| Element           | Convention                | Example                                      |
|-------------------|---------------------------|----------------------------------------------|
| Classes/Structs   | PascalCase                | `Receiver`, `AlcPacket`, `FecOti`            |
| Public methods    | snake_case                | `file_list()`, `send_next_packet()`          |
| Private methods   | `_snake_case` (prefix)    | `_attach_file()`, `_free_file_data()`        |
| Member variables  | `_snake_case` (prefix)    | `_socket`, `_files_mutex`                    |
| Local variables   | snake_case                | `bytes_recvd`, `source_block_number`         |
| Struct fields     | snake_case (no prefix)    | `encoding_id`, `transfer_length`             |
| Scoped enums      | PascalCase values         | `ContentEncoding::ZLIB`, `FecScheme::CompactNoCode` |
| Unscoped enums    | UPPER_SNAKE_CASE          | `FDT_NS_RFC3926`, `COMPRESSION_GZIP`        |
| Type aliases      | snake_case with `_t`      | `completion_callback_t`                      |
| Constants         | snake_case or enum value  | `max_length`                                 |

### Header Guards

Always use `#pragma once`. Do not use `#ifndef`/`#define` guards for project headers.

### Namespaces

- Primary namespace: `LibFlute` (PascalCase)
- Nested namespaces use C++17 syntax: `namespace LibFlute::IpSec { ... }`
- Use anonymous namespaces for file-local helpers in `.cpp` files

### Return Types

- **Header files (.h):** Conventional return type syntax: `void enable_ipsec(...);`
- **Source files (.cpp):** Trailing return type syntax: `auto Receiver::enable_ipsec(...) -> void`

### Indentation and Formatting

- 2 spaces, no tabs
- Google format style (per `.clang-tidy` FormatStyle)
- K&R brace style for control structures and short functions
- Constructor initializer lists: one member per line, leading comma, opening brace on new line

### Includes

No strictly enforced order. Prefer this grouping:
1. Own header (e.g., `#include "Receiver.h"`)
2. Project headers (e.g., `#include "AlcPacket.h"`)
3. Standard library (e.g., `<string>`, `<iostream>`)
4. Third-party (e.g., `"spdlog/spdlog.h"`, Boost headers)

### Error Handling

- The codebase historically uses `throw "string literal"` for errors in library code.
  When modifying existing code, follow the local pattern for consistency.
- Use `spdlog` for logging (`spdlog::error(...)`, `spdlog::warn(...)`, `spdlog::debug(...)`, etc.)
- Boost `error_code` is used in async receive/send handlers
- Catch exceptions by const reference: `catch (const std::exception& ex)`

### Memory Management

- Use `std::shared_ptr` / `std::make_shared` for shared ownership of objects (files, FDT)
- Use `std::unique_ptr` / `std::make_unique` for sole ownership
- Raw `malloc`/`free` and `new[]`/`delete[]` are used for low-level buffer management
- Do not introduce raw `new`/`delete` for object ownership; prefer smart pointers

### Documentation

- Every file starts with the 5G-MAG license header (15-line `//` comment block)
- Public API in headers uses Doxygen `/** ... */` with `@param`, `@return`, `@see` tags
- Use `//` for inline comments in implementation files
- Mark unfinished work with `// [TODO]` comments

### Threading

- Protect shared state with `std::mutex` and `std::lock_guard`
- Pattern: `const std::lock_guard<std::mutex> lock(_files_mutex);`
