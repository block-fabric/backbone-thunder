# One test binary: Thunder's cases plus the core's (bt_core_test_cases from core/).
add_executable(bt_tests
  test_thunder_types.cpp
  test_thunder_replay.cpp
  test_thunder_quirks.cpp)
target_link_libraries(bt_tests PRIVATE bt_test_harness bt_core_test_cases thunder_core bt_core)
target_compile_definitions(bt_tests PRIVATE BT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME bt_tests COMMAND bt_tests)
if(BT_WITH_NET)
  target_link_libraries(bt_tests PRIVATE bt_net)
endif()
