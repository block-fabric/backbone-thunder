# Pinned third-party builds for backbone-thunder (static), driven through CMake so that
# ~/skydoge/tools/build.sh can run them like any other build:
#   cmake -S depends -B depends/work/<host> [-DCMAKE_TOOLCHAIN_FILE=../cmake/mingw64.cmake]
#   ~/skydoge/tools/build.sh ~/skydoge/backbone-thunder depends/work/<host>
# Output prefix: depends/built/<host>. Tarballs are verified against the published SHA-256.
cmake_minimum_required(VERSION 3.22)
project(bt_depends NONE)
include(ExternalProject)

set(BT_HOST "linux" CACHE STRING "Host triple label (linux | mingw64)")
set(PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/built/${BT_HOST}")
set(SRC "${CMAKE_CURRENT_SOURCE_DIR}/sources")

set(OPENSSL_VERSION 3.5.9)
set(OPENSSL_SHA256 603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a)
set(NGTCP2_VERSION 1.25.0)
set(NGTCP2_SHA256 2a34d2484ba17847a5d11965704e9dd0fac4c6d8efc75ffe1ec7de66d8c6b6fb)
# Only needed when cross-compiling (Linux builds use the distribution packages):
set(SODIUM_VERSION 1.0.20)
set(SODIUM_SHA256 ebb65ef6ca439333c2bb41a0c1990587288da07f6c7fd07cb3a18cc18d30ce19)
set(SQLITE_VERSION 3500400)
set(SQLITE_SHA256 a3db587a1b92ee5ddac2f66b3edb41b26f9c867275782d46c3a088977d6a5b18)
set(NGHTTP2_VERSION 1.70.0)
set(NGHTTP2_SHA256 e05cb1388eaca3830aded4ccf20044b6e1ac1a61411dcca11b0437c4285c8bc2)

if(BT_HOST STREQUAL "mingw64")
  set(OPENSSL_TARGET mingw64 --cross-compile-prefix=x86_64-w64-mingw32- no-asm)
  set(CROSS_ARGS -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
else()
  set(OPENSSL_TARGET linux-x86_64)
  set(CROSS_ARGS)
endif()

# OpenSSL 3.5 LTS (Apache-2.0): TLS 1.3 + the third-party QUIC TLS API ngtcp2 uses.
ExternalProject_Add(openssl
  URL ${SRC}/openssl-${OPENSSL_VERSION}.tar.gz
  URL_HASH SHA256=${OPENSSL_SHA256}
  CONFIGURE_COMMAND <SOURCE_DIR>/Configure ${OPENSSL_TARGET} --prefix=${PREFIX} --libdir=lib
                    no-shared no-tests no-docs no-apps no-module no-legacy no-dso no-engine
  BUILD_COMMAND $(MAKE) build_libs
  INSTALL_COMMAND $(MAKE) install_dev
  BUILD_IN_SOURCE 1)

# ngtcp2 (MIT): QUIC transport, with its ngtcp2_crypto_ossl backend.
ExternalProject_Add(ngtcp2
  DEPENDS openssl
  URL ${SRC}/ngtcp2-${NGTCP2_VERSION}.tar.xz
  URL_HASH SHA256=${NGTCP2_SHA256}
  CMAKE_ARGS ${CROSS_ARGS} -DCMAKE_INSTALL_PREFIX=${PREFIX} -DCMAKE_BUILD_TYPE=Release
             -DENABLE_LIB_ONLY=ON -DENABLE_SHARED_LIB=OFF -DENABLE_STATIC_LIB=ON
             -DENABLE_OPENSSL=ON -DOPENSSL_ROOT_DIR=${PREFIX} -DOPENSSL_USE_STATIC_LIBS=ON
             -DCMAKE_POSITION_INDEPENDENT_CODE=ON)

if(BT_HOST STREQUAL "mingw64")
  set(XHOST x86_64-w64-mingw32)
  set(XENV CC=x86_64-w64-mingw32-gcc-posix CXX=x86_64-w64-mingw32-g++-posix)
  ExternalProject_Add(libsodium
    URL ${SRC}/libsodium-${SODIUM_VERSION}.tar.gz
    URL_HASH SHA256=${SODIUM_SHA256}
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env ${XENV} <SOURCE_DIR>/configure --host=${XHOST} --prefix=${PREFIX}
                      --disable-shared --enable-static --enable-minimal=no
    BUILD_COMMAND $(MAKE)
    INSTALL_COMMAND $(MAKE) install)
  ExternalProject_Add(sqlite
    URL ${SRC}/sqlite-autoconf-${SQLITE_VERSION}.tar.gz
    URL_HASH SHA256=${SQLITE_SHA256}
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env ${XENV} <SOURCE_DIR>/configure --host=${XHOST} --prefix=${PREFIX}
                      --disable-shared --enable-static
    BUILD_COMMAND $(MAKE)
    INSTALL_COMMAND $(MAKE) install
    BUILD_IN_SOURCE 1)
  ExternalProject_Add(nghttp2
    URL ${SRC}/nghttp2-${NGHTTP2_VERSION}.tar.xz
    URL_HASH SHA256=${NGHTTP2_SHA256}
    CMAKE_ARGS ${CROSS_ARGS} -DCMAKE_INSTALL_PREFIX=${PREFIX} -DCMAKE_BUILD_TYPE=Release
               -DENABLE_LIB_ONLY=ON -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DENABLE_DOC=OFF -DBUILD_TESTING=OFF)
endif()
