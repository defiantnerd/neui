# Tier 1 unit tests - pure portable logic in hosts/shared/*.h.
#
# These link NOTHING (no host, no backend): every shared header is inline /
# header-only, so the test target only needs the public headers (include/)
# and the shared utility dir on the include path. This builds and runs on
# every platform, including the null-platform "Other" target / CI.
add_executable(neui_tests
  test_main.cpp
  test_grid_model.cpp
  test_compound.cpp
  test_behavior.cpp
  test_attrs.cpp
  test_edit_history.cpp
  test_tree.cpp
  test_scrollbar.cpp
  test_shortcut_format.cpp)

target_include_directories(neui_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/hosts/shared)

target_compile_features(neui_tests PRIVATE cxx_std_17)

add_test(NAME neui_tests COMMAND neui_tests)
