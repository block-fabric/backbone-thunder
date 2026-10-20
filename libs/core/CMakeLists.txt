# libbt_core: chain-independent pieces shared by the five LayerTwo-family chains.
add_library(bt_core STATIC
  src/bytes.cpp
  src/codec.cpp
  src/crypto.cpp
  src/bitcoin.cpp
  src/frost.cpp
  src/utreexo.cpp
  src/kv.cpp
  src/mainchain.cpp)
target_include_directories(bt_core PUBLIC include)
target_link_libraries(bt_core PUBLIC bt_blake3 bt_univalue PkgConfig::SODIUM PkgConfig::SQLITE)
