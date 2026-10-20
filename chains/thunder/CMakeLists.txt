# Thunder (eCash slot 9) rule module and binaries.
add_library(thunder_core STATIC
  src/types.cpp
  src/state.cpp
  src/betanet_fixture.cpp)
target_include_directories(thunder_core PUBLIC include)
target_link_libraries(thunder_core PUBLIC bt_core)
