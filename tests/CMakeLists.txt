# Tier 1 unit tests - pure portable logic in hosts/shared/*.h.
#
# These link NOTHING (no host, no backend): every shared header is inline /
# header-only, so the test target only needs the public headers (include/)
# and the shared utility dir on the include path. This builds and runs on
# every platform, including the null-platform "Other" target / CI.
add_executable(neui_tests
  test_main.cpp
  test_grid_model.cpp
  test_grid_sort.cpp
  test_compound.cpp
  test_behavior.cpp
  test_attrs.cpp
  test_edit_history.cpp
  test_text_edit.cpp
  test_tree.cpp
  test_scrollbar.cpp
  test_section_scroll.cpp
  test_shortcut_format.cpp
  test_clipboard_item.cpp)

target_include_directories(neui_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/hosts/shared)

target_compile_features(neui_tests PRIVATE cxx_std_17)

add_test(NAME neui_tests COMMAND neui_tests)

# Linux-only: Cairo software backend offscreen smoke test. Unlike neui_tests
# this links a real backend (neui-backend-cairo), so it is gated to the
# platforms where that backend is built.
if(UNIX AND NOT APPLE)
  add_executable(neui_cairo_smoke cairo_backend_smoke.cpp)
  target_include_directories(neui_cairo_smoke PRIVATE ${CMAKE_SOURCE_DIR}/include)
  target_compile_features(neui_cairo_smoke PRIVATE cxx_std_17)
  target_link_libraries(neui_cairo_smoke PRIVATE neui-backend-cairo)
  add_test(NAME neui_cairo_smoke COMMAND neui_cairo_smoke)

  # Phase 3 embedding harness (fake DAW host). Needs a live X display, so it
  # is built but NOT registered with ctest (keeps headless `ctest` runs green);
  # run ./tests/neui_embed_smoke manually on an X desktop.
  add_executable(neui_embed_smoke embed_smoke.cpp)
  target_include_directories(neui_embed_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/hosts/crossplatform
    ${CMAKE_SOURCE_DIR}/hosts/shared)
  target_compile_features(neui_embed_smoke PRIVATE cxx_std_17)
  target_link_libraries(neui_embed_smoke PRIVATE neui neui-xplhost neui-backend-cairo)
endif()
