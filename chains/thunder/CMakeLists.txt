# Thunder (eCash slot 9) rule module and binaries.
add_library(thunder_core STATIC
  src/types.cpp
  src/state.cpp
  src/betanet_fixture.cpp)
target_include_directories(thunder_core PUBLIC include)
target_link_libraries(thunder_core PUBLIC bt_core)
target_sources(thunder_core PRIVATE src/net.cpp)

if(BT_WITH_NET)
  # Fetch full blocks from public betanet peers over their QUIC protocol (client only).
  add_executable(thunder-fetch-blocks tools/fetch_blocks.cpp)
  target_link_libraries(thunder-fetch-blocks PRIVATE thunder_core bt_net)
endif()
