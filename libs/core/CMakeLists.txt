# libbt_core: chain-independent pieces shared by the five LayerTwo-family chains.
add_library(bt_core STATIC
  src/bytes.cpp
  src/codec.cpp
  src/crypto.cpp
  src/bitcoin.cpp
  src/frost.cpp
  src/utreexo.cpp
  src/kv.cpp
  src/mainchain.cpp
  src/protobuf.cpp
  src/hd.cpp
  src/log.cpp
  src/jsonrpc.cpp)
target_include_directories(bt_core PUBLIC include)
target_link_libraries(bt_core PUBLIC bt_blake3 bt_univalue PkgConfig::SODIUM PkgConfig::SQLITE)

# libbt_net: QUIC transport (ngtcp2 + OpenSSL 3.5), kept apart so the consensus core
# carries no TLS dependency.
if(BT_WITH_NET)
  find_library(NGTCP2_LIB ngtcp2 REQUIRED)
  find_library(NGTCP2_CRYPTO_LIB ngtcp2_crypto_ossl REQUIRED)
  find_path(NGTCP2_INCLUDE ngtcp2/ngtcp2.h REQUIRED)
  set(OPENSSL_USE_STATIC_LIBS ON)
  if(BT_DEPENDS_PREFIX)
    set(OPENSSL_ROOT_DIR ${BT_DEPENDS_PREFIX})
  endif()
  find_package(OpenSSL 3.5 REQUIRED)
  pkg_check_modules(NGHTTP2 REQUIRED IMPORTED_TARGET libnghttp2)
  add_library(bt_net STATIC src/quic.cpp src/grpc.cpp src/enforcer.cpp)
  target_include_directories(bt_net PUBLIC include ${NGTCP2_INCLUDE})
  target_compile_definitions(bt_net PRIVATE NGTCP2_STATICLIB)
  target_link_libraries(bt_net PUBLIC bt_core ${NGTCP2_CRYPTO_LIB} ${NGTCP2_LIB} PkgConfig::NGHTTP2 OpenSSL::SSL OpenSSL::Crypto Threads::Threads ${CMAKE_DL_LIBS})
  if(WIN32)
    target_link_libraries(bt_net PUBLIC ws2_32 crypt32)
  endif()
endif()
