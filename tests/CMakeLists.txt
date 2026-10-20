add_executable(bt_tests
  main.cpp
  test_core.cpp
  test_thunder_types.cpp
  test_thunder_replay.cpp)
target_link_libraries(bt_tests PRIVATE thunder_core bt_core)
target_compile_definitions(bt_tests PRIVATE BT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME bt_tests COMMAND bt_tests)
