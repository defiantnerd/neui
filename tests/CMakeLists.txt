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
  test_clipboard_item.cpp
  test_asset_font.cpp
  test_asset_filmstrip.cpp
  test_painter_text.cpp
  test_timer_table.cpp
  test_cursor_kind.cpp
  test_relative_pointer.cpp
  test_file_dialog_model.cpp
  test_image_filter.cpp
  test_filter_graph.cpp
  test_backend_util.cpp
  test_tabview.cpp
  test_mujson.cpp
  test_component_loader.cpp
  # mujson lives in the core lib, but the suite links no library, so its TU is
  # compiled directly into the test executable (same pattern as the *.cpp above).
  ${PROJECT_SOURCE_DIR}/src/mujson.cpp)

target_include_directories(neui_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/hosts/shared
  ${PROJECT_SOURCE_DIR}/src)

target_compile_features(neui_tests PRIVATE cxx_std_17)

# iOS: the suite is a console-style program, but iOS only installs/launches app
# bundles - give it a deterministic bundle id so CI can install it on a booted
# simulator, launch it by id, and scrape the pass/fail summary from the console.
if(NEUI_IOS)
  set_target_properties(neui_tests PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.neui.tests"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "org.neui.tests")
endif()

add_test(NAME neui_tests COMMAND neui_tests)

# macOS-only: DAW-embedding harness (fake host NSWindow + NSView slot,
# embeds a PLUGWINDOW via NEUI_API_EMBED). Needs a GUI session, so it is
# built but NOT registered with ctest; run
# ./tests/<config>/neui_embed_smoke_macos manually.
if(APPLE AND NOT NEUI_IOS)
  add_executable(neui_embed_smoke_macos embed_smoke_macos.mm)
  target_include_directories(neui_embed_smoke_macos PRIVATE
    ${PROJECT_SOURCE_DIR}/include)
  target_compile_features(neui_embed_smoke_macos PRIVATE cxx_std_17)
  target_link_libraries(neui_embed_smoke_macos PRIVATE
    neui neui-xplhost neui-macoshost "-framework AppKit")

  # UI-zoom harness (NEUI_ATTR_UI_SCALE): asserts native sizing grows by the
  # zoom while the client's logical view doesn't, and that a CUSTOMDRAW gets
  # logical vs device-pixel dimensions per NEUI_ATTR_PAINT_DEVICE_PIXELS.
  # Realizes a real NSWindow, so built but NOT ctest-registered.
  add_executable(neui_zoom_smoke_macos zoom_smoke_macos.mm)
  target_include_directories(neui_zoom_smoke_macos PRIVATE
    ${PROJECT_SOURCE_DIR}/include)
  target_compile_features(neui_zoom_smoke_macos PRIVATE cxx_std_17)
  target_link_libraries(neui_zoom_smoke_macos PRIVATE
    neui neui-xplhost neui-macoshost "-framework AppKit")

  # Cursor harness (NEUI_ATTR_CURSOR): asserts inheritance up the widget tree,
  # live attr writes under the pointer, that -cursorUpdate: defeats AppKit's
  # cursor reset, and that the GRID's positional override neither breaks nor
  # leaks. Realizes a real NSWindow, so built but NOT ctest-registered.
  add_executable(neui_cursor_smoke_macos cursor_smoke_macos.mm)
  target_include_directories(neui_cursor_smoke_macos PRIVATE
    ${PROJECT_SOURCE_DIR}/include)
  target_compile_features(neui_cursor_smoke_macos PRIVATE cxx_std_17)
  target_link_libraries(neui_cursor_smoke_macos PRIVATE
    neui neui-xplhost neui-macoshost "-framework AppKit")

  # Accessibility-declaration harness (NEUI_API_A11Y): asserts the interface is
  # present on the xpl host and NULL on the native macOS host (the documented
  # feature-detect trap), that every clearing path removes its key rather than
  # storing an empty value, that a degenerate min==max range and an out-of-range
  # or NaN normalized value are handled at the door, that set_state stores only
  # in-mask bits, and that cross-session handles are dropped. Shows no window, so
  # it needs no GUI session - but it links the host, so it cannot be Tier-1. Built
  # but NOT ctest-registered, for consistency with the other harnesses.
  add_executable(neui_a11y_smoke_macos a11y_smoke_macos.mm)
  target_include_directories(neui_a11y_smoke_macos PRIVATE
    ${PROJECT_SOURCE_DIR}/include)
  target_compile_features(neui_a11y_smoke_macos PRIVATE cxx_std_17)
  target_link_libraries(neui_a11y_smoke_macos PRIVATE
    neui neui-xplhost neui-macoshost "-framework AppKit")

  # Focus-traversal harness: asserts TAB traversal is per FRAME (it used to
  # collect tab stops across every frame in the session, so tabbing off the last
  # control of one window moved the logical focus into another window while the
  # OS keyboard focus stayed put), that reverse traversal and wraparound also
  # stay in-frame, that a second frame cycles independently, and that a click
  # inside a chip-bearing SECTION's child still reports the child with correct
  # widget-local coordinates (guarding the shared child-origin helper). Realizes
  # two real NSWindows, so built but NOT ctest-registered.
  add_executable(neui_focus_smoke_macos focus_smoke_macos.mm)
  target_include_directories(neui_focus_smoke_macos PRIVATE
    ${PROJECT_SOURCE_DIR}/include)
  target_compile_features(neui_focus_smoke_macos PRIVATE cxx_std_17)
  target_link_libraries(neui_focus_smoke_macos PRIVATE
    neui neui-xplhost neui-macoshost "-framework AppKit")

  # Tree-model context-menu harness (NEUI_W_POPUPMENU + popup_tree_menu):
  # asserts a POPUPMENU never becomes the frame's menu bar (read from
  # [NSApp mainMenu]), that its ROOT rows are ordinary pickable rows with
  # working separators / checkmarks / command routing, that a pick routes to the
  # popup rather than a menu bar holding the same cmd id, that nothing leaks
  # through to the widget beneath, and that clear / empty / destroy while open
  # leave no invisible modal grab. Realizes a real NSWindow, so built but NOT
  # ctest-registered.
  add_executable(neui_popup_tree_smoke_macos popup_tree_smoke_macos.mm)
  target_include_directories(neui_popup_tree_smoke_macos PRIVATE
    ${PROJECT_SOURCE_DIR}/include)
  target_compile_features(neui_popup_tree_smoke_macos PRIVATE cxx_std_17)
  target_link_libraries(neui_popup_tree_smoke_macos PRIVATE
    neui neui-xplhost neui-macoshost "-framework AppKit")

  # File-dialog harness (NEUI_API_NOTIFY::open_file / save_file): asserts the
  # appended slots are wired on BOTH macOS hosts, that -1 (no dialog) stays
  # distinct from 0 (cancelled), that a cancelled panel delivers nothing, and
  # that a confirmed save panel returns AppKit's own path with the filter
  # extension completed. Runs real modal NSSavePanel / NSOpenPanel, so built
  # but NOT ctest-registered.
  add_executable(neui_file_dialog_smoke_macos file_dialog_smoke_macos.mm)
  target_include_directories(neui_file_dialog_smoke_macos PRIVATE
    ${PROJECT_SOURCE_DIR}/include)
  target_compile_features(neui_file_dialog_smoke_macos PRIVATE cxx_std_17)
  target_link_libraries(neui_file_dialog_smoke_macos PRIVATE
    neui neui-xplhost neui-macoshost "-framework AppKit")

  # Repaint throughput benchmark (wave 5 / Phase 0a): times one full-frame
  # repaint of an NxN CUSTOMDRAW mesh on the xpl host and reports it against a
  # 60 Hz budget, plus the single-widget-invalidate baseline that partial
  # repaint has to improve on. Realizes a real window, so built but NOT
  # ctest-registered; run ./tests/<config>/neui_repaint_bench.
  add_executable(neui_repaint_bench repaint_bench.cpp)
  target_include_directories(neui_repaint_bench PRIVATE
    ${PROJECT_SOURCE_DIR}/include)
  target_compile_features(neui_repaint_bench PRIVATE cxx_std_17)
  target_link_libraries(neui_repaint_bench PRIVATE
    neui neui-xplhost neui-macoshost "-framework AppKit")

  # Relative-pointer harness (NEUI_API_POINTER): asserts the begin/end state
  # machine, that MOUSE_MOVE coordinates are unbounded and widget-local, that
  # sub-pixel deltas accumulate, and that a session destroyed mid-drag leaves the
  # cursor re-associated. Realizes a real NSWindow and warps the pointer, so built
  # but NOT ctest-registered.
  add_executable(neui_pointer_smoke_macos pointer_smoke_macos.mm)
  target_include_directories(neui_pointer_smoke_macos PRIVATE
    ${PROJECT_SOURCE_DIR}/include)
  target_compile_features(neui_pointer_smoke_macos PRIVATE cxx_std_17)
  target_link_libraries(neui_pointer_smoke_macos PRIVATE
    neui neui-xplhost neui-macoshost
    "-framework AppKit" "-framework CoreGraphics")
endif()

# Linux-only: Cairo software backend offscreen smoke test. Unlike neui_tests
# this links a real backend (neui-backend-cairo), so it is gated to the
# platforms where that backend is built.
if(UNIX AND NOT APPLE)
  add_executable(neui_cairo_smoke cairo_backend_smoke.cpp)
  target_include_directories(neui_cairo_smoke PRIVATE ${PROJECT_SOURCE_DIR}/include)
  target_compile_features(neui_cairo_smoke PRIVATE cxx_std_17)
  target_link_libraries(neui_cairo_smoke PRIVATE neui-backend-cairo)
  add_test(NAME neui_cairo_smoke COMMAND neui_cairo_smoke)

  # Phase 3 embedding harness (fake DAW host). Needs a live X display, so it
  # is built but NOT registered with ctest (keeps headless `ctest` runs green);
  # run ./tests/neui_embed_smoke manually on an X desktop.
  add_executable(neui_embed_smoke embed_smoke.cpp)
  target_include_directories(neui_embed_smoke PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/hosts/crossplatform
    ${PROJECT_SOURCE_DIR}/hosts/shared)
  target_compile_features(neui_embed_smoke PRIVATE cxx_std_17)
  target_link_libraries(neui_embed_smoke PRIVATE neui neui-xplhost neui-backend-cairo)

  # Repaint benchmark, Linux/Cairo. Same source as the macOS build above - the
  # file is platform-neutral (it asks for neui.host.crossplatform and uses only
  # the public API). This is the backend where the answer could differ: Cairo is
  # a SOFTWARE renderer whose begin_frame clears the whole surface and whose
  # end_frame blits it via XShm, so its cost scales with PIXELS as well as with
  # widget count - unlike the GPU-backed CG path the macOS number came from.
  # Needs a live X display, so built but not ctest-registered; run
  # ./tests/neui_repaint_bench manually.
  add_executable(neui_repaint_bench repaint_bench.cpp)
  target_include_directories(neui_repaint_bench PRIVATE
    ${PROJECT_SOURCE_DIR}/include)
  target_compile_features(neui_repaint_bench PRIVATE cxx_std_17)
  target_link_libraries(neui_repaint_bench PRIVATE neui neui-xplhost)

  # XDND drag-source harness (internal-drag path). Needs a live X display AND
  # warps the pointer, so it is built but not ctest-registered; run
  # ./tests/neui_dnd_source_smoke manually.
  find_package(Threads REQUIRED)
  add_executable(neui_dnd_source_smoke dnd_source_smoke.cpp)
  target_include_directories(neui_dnd_source_smoke PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/hosts/crossplatform
    ${PROJECT_SOURCE_DIR}/hosts/shared)
  target_compile_features(neui_dnd_source_smoke PRIVATE cxx_std_17)
  target_link_libraries(neui_dnd_source_smoke PRIVATE
    neui neui-xplhost neui-backend-cairo Threads::Threads)

  # Clipboard TARGETS regression (LP64 format-32 property read). Stands up a
  # foreign CLIPBOARD owner on a second X connection; needs a live X display,
  # so it is built but not ctest-registered. Header-only (clipboard_linux.h),
  # links no host/backend.
  add_executable(neui_clipboard_targets_smoke clipboard_targets_smoke.cpp)
  target_include_directories(neui_clipboard_targets_smoke PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/hosts/shared)
  target_compile_features(neui_clipboard_targets_smoke PRIVATE cxx_std_17)
  find_package(X11 REQUIRED)
  target_link_libraries(neui_clipboard_targets_smoke PRIVATE
    ${X11_LIBRARIES} Threads::Threads)

  # INCR chunked-transfer round-trip (two ClipboardX11 instances over a ~1 MB
  # payload). Needs a live X display; built but not ctest-registered.
  add_executable(neui_incr_smoke incr_smoke.cpp)
  target_include_directories(neui_incr_smoke PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/hosts/shared)
  target_compile_features(neui_incr_smoke PRIVATE cxx_std_17)
  target_link_libraries(neui_incr_smoke PRIVATE ${X11_LIBRARIES} Threads::Threads)

  # Message-box harness (drives modal boxes from a background thread). Needs a
  # live X display; built but not ctest-registered.
  add_executable(neui_notify_smoke notify_smoke.cpp)
  target_include_directories(neui_notify_smoke PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/hosts/crossplatform
    ${PROJECT_SOURCE_DIR}/hosts/shared)
  target_compile_features(neui_notify_smoke PRIVATE cxx_std_17)
  target_link_libraries(neui_notify_smoke PRIVATE
    neui neui-xplhost neui-backend-cairo Threads::Threads)

  # In-frame menubar harness (drives synthetic clicks + accelerators at the
  # frame and asserts the activated item). Needs a live X display + cooperative
  # WM; built but not ctest-registered. Run ./tests/neui_menubar_smoke.
  add_executable(neui_menubar_smoke menubar_smoke.cpp)
  target_include_directories(neui_menubar_smoke PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/hosts/crossplatform
    ${PROJECT_SOURCE_DIR}/hosts/shared)
  target_compile_features(neui_menubar_smoke PRIVATE cxx_std_17)
  target_link_libraries(neui_menubar_smoke PRIVATE
    neui neui-xplhost neui-backend-cairo Threads::Threads)
endif()
