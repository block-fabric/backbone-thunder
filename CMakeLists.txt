cmake_minimum_required(VERSION 3.22)
project(backbone_thunder VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(BT_STATIC "Link third-party libraries statically" ON)
option(BT_BUILD_TESTS "Build unit tests" ON)
option(BT_WITH_NET "Build the QUIC/gRPC/RPC node (needs OpenSSL>=3.5, ngtcp2, nghttp2, libevent)" ON)
set(BT_DEPENDS_PREFIX "" CACHE PATH "Prefix with our pinned third-party builds (depends/)")
if(BT_DEPENDS_PREFIX)
  list(PREPEND CMAKE_PREFIX_PATH "${BT_DEPENDS_PREFIX}")
  list(PREPEND CMAKE_FIND_ROOT_PATH "${BT_DEPENDS_PREFIX}")
endif()

# Consensus code must not use contracted floating point (fma) anywhere: the f64 block
# limits must equal Rust's non-fused results.
add_compile_options(-Wall -Wextra -Wno-unused-parameter $<$<COMPILE_LANGUAGE:C,CXX>:-ffp-contract=off>)

find_package(PkgConfig REQUIRED)
if(BT_STATIC)
  set(_pc_static_flag STATIC)
endif()
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(SQLITE REQUIRED IMPORTED_TARGET sqlite3)

# ---------------------------------------------------------------- third party
add_library(bt_blake3 STATIC
  third_party/blake3/blake3.c
  third_party/blake3/blake3_dispatch.c
  third_party/blake3/blake3_portable.c)
target_include_directories(bt_blake3 PUBLIC third_party/blake3)
target_compile_definitions(bt_blake3 PRIVATE BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512 BLAKE3_USE_NEON=0)

add_library(bt_univalue STATIC
  third_party/univalue/lib/univalue.cpp
  third_party/univalue/lib/univalue_get.cpp
  third_party/univalue/lib/univalue_read.cpp
  third_party/univalue/lib/univalue_write.cpp)
target_include_directories(bt_univalue PUBLIC third_party/univalue/include)
target_compile_options(bt_univalue PRIVATE -w)

# ---------------------------------------------------------------- core
add_subdirectory(libs/core)
add_subdirectory(chains/thunder)

if(BT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
