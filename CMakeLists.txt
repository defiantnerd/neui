cmake_minimum_required(VERSION 3.15)
project(neui LANGUAGES C CXX)

# Whether neui is being configured standalone (its own CMakeLists is the top
# of the tree) vs. pulled in as a subproject via add_subdirectory(). The
# examples + tests are only interesting to people hacking on neui itself, so
# they default ON standalone and OFF when neui is consumed as a dependency.
# (PROJECT_IS_TOP_LEVEL is only available from CMake 3.21, but our floor is
# 3.15, so compute it by hand.)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(NEUI_IS_TOP_LEVEL ON)
else()
  set(NEUI_IS_TOP_LEVEL OFF)
endif()

option(NEUI_BUILD_EXAMPLES "Build the neui example programs" ${NEUI_IS_TOP_LEVEL})
option(NEUI_BUILD_TESTS    "Build the neui unit tests"       ${NEUI_IS_TOP_LEVEL})

# iOS is an APPLE platform too, so it must be detected distinctly from macOS:
# the CoreGraphics backend is shared (gated on APPLE), but the AppKit macOS
# host must NOT be built for iOS, and the UIKit iOS host is built only here.
# CMAKE_SYSTEM_NAME is "iOS" when configuring with -DCMAKE_SYSTEM_NAME=iOS
# (CMake's built-in iOS toolchain support).
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(NEUI_IOS ON)
else()
  set(NEUI_IOS OFF)
endif()

# Objective-C++ is needed for the macOS / iOS host platform layers (.mm files).
if(APPLE)
  enable_language(OBJCXX)
endif()

# Set C++ standard to C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Warnings-as-linter. The project ships no dedicated static-analysis tool, so
# the compiler's own diagnostics are the safety net - turn them up. On MSVC we
# strip the default /W3 first so cl doesn't emit D9025 ("overriding /W3 with
# /W4") on every translation unit; GCC / AppleClang get the equivalent broad
# set. Warnings are not errors (no /WX or -Werror) - this surfaces issues
# without breaking the build. Directory-scoped, so embedding neui as a
# subproject doesn't disturb the parent's own flags.
# C4100 / -Wunused-parameter is suppressed: every occurrence here is a
# fixed-signature parameter that has nothing to fix - base-class virtual no-op
# defaults (on_keydown / on_keychar / on_mouse_event), the Win32 WinMain and
# SUBCLASSPROC signatures, callback stubs - where the name documents the slot.
# Keeping it off leaves the baseline warning-clean so a genuinely new warning
# (a real unused variable, a narrowing conversion) actually stands out.
if(MSVC)
  string(REGEX REPLACE "/W[0-4]" "" CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}")
  string(REGEX REPLACE "/W[0-4]" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  add_compile_options(/W4 /wd4100)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# The registry lib (neui) and the host libs reference each other's symbols:
# neui calls each host's register wrapper, and those wrappers double as the
# forced-symbol refs that pull the host objects out of their static archives.
# That cycle is intentional, but it means a consumer's link line lists
# libneui.a twice - once explicitly, once propagated through a host lib's
# PRIVATE dependency on neui. The link is correct (the linker drops the
# duplicate), but Apple's new linker (ld_prime, Xcode 15+) emits an
# "ignoring duplicate libraries" warning for it. Silence that one cosmetic
# warning where the linker accepts the flag. (CheckLinkerFlag needs CMake
# 3.18; our floor is 3.15, so guard the version too.)
if(APPLE AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.18)
  include(CheckLinkerFlag)
  check_linker_flag(CXX "-Wl,-no_warn_duplicate_libraries"
                    NEUI_LD_NO_WARN_DUP_LIBS)
  if(NEUI_LD_NO_WARN_DUP_LIBS)
    add_link_options("-Wl,-no_warn_duplicate_libraries")
  endif()
endif()

# build a library with a appropriate host. mujson (src/mujson.cpp) is a small
# JSON-like parser that belongs to the core lib (neui::mujson) - C++ alongside
# the C registry; CMake compiles each TU in its own language.
add_library(neui src/neui.c src/mujson.cpp)

# Per-platform host + backend selection.
#   Windows : neui-win32host (native HWND host) + neui-xplhost (D2D-rendered)
#   macOS   : neui-xplhost (CoreGraphics-rendered) - neui-win32host is omitted
#   Other   : neui-xplhost (null backend)          - same
if(WIN32)
  add_subdirectory("hosts/win32")
  add_subdirectory("backends/d2d")
endif()
# CoreGraphics backend covers both macOS and iOS.
if(APPLE)
  add_subdirectory("backends/cg")
endif()
# Native AppKit host - macOS only (NOT iOS, which uses the UIKit host below).
if(APPLE AND NOT NEUI_IOS)
  add_subdirectory("hosts/macos")
endif()
# Native UIKit host - iOS only.
if(NEUI_IOS)
  add_subdirectory("hosts/ios")
endif()
if(UNIX AND NOT APPLE)
  add_subdirectory("backends/cairo")
endif()
add_subdirectory("backends/null")
add_subdirectory("hosts/crossplatform")

# Tier 1 unit tests for the portable shared logic (hosts/shared/*.h). They
# depend only on the public headers + the shared utilities, so they build
# everywhere. Defaults ON when neui is the top-level project, OFF as a
# subproject; override with -DNEUI_BUILD_TESTS=ON/OFF (option defined above).
if(NEUI_BUILD_TESTS)
  enable_testing()
  add_subdirectory("tests")
endif()

# neui_init() (src/neui.c) is gated on these defines so it
# only references registration wrappers for hosts that are actually being
# built. The defines are PRIVATE: they're an implementation detail of the
# core lib's startup helper, not part of the public C interface.
target_compile_definitions(neui PRIVATE NEUI_HAS_XPLHOST)
if(WIN32)
  target_compile_definitions(neui PRIVATE NEUI_HAS_WIN32HOST)
endif()
if(APPLE AND NOT NEUI_IOS)
  target_compile_definitions(neui PRIVATE NEUI_HAS_MACOSHOST)
endif()
if(NEUI_IOS)
  target_compile_definitions(neui PRIVATE NEUI_HAS_IOSHOST)
endif()

# provide public headers
target_include_directories(neui
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# versioning
set_target_properties(neui PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)

# Example programs. Defaults ON when neui is the top-level project, OFF as a
# subproject; override with -DNEUI_BUILD_EXAMPLES=ON/OFF (option defined above).
# The desktop examples are plain executables with no app bundle / Info.plist /
# scene manifest, so they cannot link or run on iOS - gate them off there.
# A dedicated iOS example app target is a later milestone.
if(NEUI_BUILD_EXAMPLES AND NOT NEUI_IOS)

# Example program
# On MSVC we provide the application manifest via an .rc file that
# embeds it as an RT_MANIFEST resource, and disable link.exe's
# auto-generated manifest with /MANIFEST:NO. The manifest declares the
# Win10/11 supportedOS GUIDs that gate dark-mode HMENU theming, plus
# UTF-8 ACP and per-monitor v2 DPI awareness. Mixing /MANIFESTINPUT with
# the auto-manifest causes "duplicate resource" link errors, so the .rc
# route (which is what MSBuild uses internally) is the clean path.
if(MSVC)
  add_executable(neui_example WIN32
    examples/main.cpp
    examples/neui_example.rc)
  target_link_options(neui_example PRIVATE "/MANIFEST:NO")
  set_property(TARGET neui_example APPEND PROPERTY
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/examples/neui_example.manifest")
  # Rebuild the .rc whenever a file it embeds changes (manifest or PNGs).
  set_property(SOURCE examples/neui_example.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/neui_example.manifest"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/myimage.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/myimage@2x.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/knob_bg.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/knob_bg@2x.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/knob_move.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/knob_move@2x.png")
elseif(WIN32)
  add_executable(neui_example WIN32 examples/main.cpp)
elseif(APPLE)
  # Bundle so AppKit can install the menubar / activate normally. The
  # example images are copied into Contents/Resources so the bundle works
  # when launched from anywhere (Xcode, Finder, `open`), not just from the
  # project root via terminal where cwd matches. `myimage@2x.png` is the
  # HiDPI variant - the asset manager picks it automatically on Retina.
  set(NEUI_EXAMPLE_RESOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/myimage.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/myimage@2x.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/knob_bg.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/knob_bg@2x.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/knob_move.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/knob_move@2x.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/lemur@2x.jpg"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/lion@2x.jpg"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/panda@2x.jpg")
  add_executable(neui_example MACOSX_BUNDLE
    examples/main.cpp
    ${NEUI_EXAMPLE_RESOURCES})
  set_source_files_properties(${NEUI_EXAMPLE_RESOURCES} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources")
  # Disable Xcode's tiffutil step that auto-combines name.png + name@2x.png
  # pairs into a single multi-rep .tiff. The runtime path-resolver in
  # hosts/shared/macos/image_loader_macos.h looks up the .png by name, so
  # we want the individual files in Resources/ verbatim. Without this the
  # bundle ends up with myimage.tiff and no myimage.png, breaking both
  # the IMAGE widget and the compound demo's background asset.
  set_target_properties(neui_example PROPERTIES
    XCODE_ATTRIBUTE_COMBINE_HIDPI_IMAGES NO)
else()
  add_executable(neui_example examples/main.cpp)
endif()

# Windows: PNGs are embedded into the executable as user-defined "PNG"
# resources by examples/neui_example.rc. The xpl host's
# platform_load_image looks them up via FindResourceW(name, L"PNG")
# before falling back to disk - no runtime file dependency on the example.

# Link the appropriate hosts. The win32 host is omitted on non-Windows;
# the xpl host is built everywhere.
target_link_libraries(neui_example PRIVATE neui neui-xplhost)
if(WIN32)
  target_link_libraries(neui_example PRIVATE neui-win32host)
endif()
if(APPLE)
  target_link_libraries(neui_example PRIVATE neui-macoshost)
endif()

# GRID widget example - standalone demo separate from the monolithic
# neui_example so the new widget can be exercised on its own.
# Reuses the same Win32 application manifest the main example uses (the
# manifest is generic; it gates dark-mode HMENU theming + per-monitor v2
# DPI + UTF-8 ACP, all of which apply identically to this app).
if(MSVC)
  add_executable(neui_grid_example WIN32
    examples/grid_example.cpp
    examples/neui_example.rc)
  target_link_options(neui_grid_example PRIVATE "/MANIFEST:NO")
  set_property(TARGET neui_grid_example APPEND PROPERTY
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/examples/neui_example.manifest")
elseif(WIN32)
  add_executable(neui_grid_example WIN32 examples/grid_example.cpp)
elseif(APPLE)
  add_executable(neui_grid_example MACOSX_BUNDLE examples/grid_example.cpp)
else()
  add_executable(neui_grid_example examples/grid_example.cpp)
endif()
target_link_libraries(neui_grid_example PRIVATE neui neui-xplhost)
if(WIN32)
  target_link_libraries(neui_grid_example PRIVATE neui-win32host)
endif()
if(APPLE)
  target_link_libraries(neui_grid_example PRIVATE neui-macoshost)
endif()

# DnD drop-receiver example. Standalone executable so the drop-target
# code path can be exercised without dragging the broader example app
# around. Reuses the same Win32 manifest as neui_example.
if(MSVC)
  add_executable(neui_dnd_example WIN32
    examples/dnd_example.cpp
    examples/neui_example.rc)
  target_link_options(neui_dnd_example PRIVATE "/MANIFEST:NO")
  set_property(TARGET neui_dnd_example APPEND PROPERTY
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/examples/neui_example.manifest")
elseif(WIN32)
  add_executable(neui_dnd_example WIN32 examples/dnd_example.cpp)
elseif(APPLE)
  add_executable(neui_dnd_example MACOSX_BUNDLE examples/dnd_example.cpp)
else()
  add_executable(neui_dnd_example examples/dnd_example.cpp)
endif()
target_link_libraries(neui_dnd_example PRIVATE neui neui-xplhost)
if(WIN32)
  target_link_libraries(neui_dnd_example PRIVATE neui-win32host)
endif()
if(APPLE)
  target_link_libraries(neui_dnd_example PRIVATE neui-macoshost)
endif()

# DnD drag-source example. Side-by-side source + target panes that
# exercise the begin_drag API end to end (and accept the drag back into
# the same window, plus into external apps).
if(MSVC)
  add_executable(neui_dnd_source_example WIN32
    examples/dnd_source_example.cpp
    examples/neui_example.rc)
  target_link_options(neui_dnd_source_example PRIVATE "/MANIFEST:NO")
  set_property(TARGET neui_dnd_source_example APPEND PROPERTY
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/examples/neui_example.manifest")
elseif(WIN32)
  add_executable(neui_dnd_source_example WIN32 examples/dnd_source_example.cpp)
elseif(APPLE)
  add_executable(neui_dnd_source_example MACOSX_BUNDLE examples/dnd_source_example.cpp)
else()
  add_executable(neui_dnd_source_example examples/dnd_source_example.cpp)
endif()
target_link_libraries(neui_dnd_source_example PRIVATE neui neui-xplhost)
if(WIN32)
  target_link_libraries(neui_dnd_source_example PRIVATE neui-win32host)
endif()
if(APPLE)
  target_link_libraries(neui_dnd_source_example PRIVATE neui-macoshost)
endif()

# Render-to-surface example. Renders a procedural bar chart into an
# off-screen SURFACE asset and displays it via a CUSTOMDRAW widget that
# calls painter->draw_asset, exercising create_surface + paint_surface
# (incl. the per-window upload-cache drop on re-render).
if(MSVC)
  add_executable(neui_surface_example WIN32
    examples/surface_example.cpp
    examples/neui_example.rc)
  target_link_options(neui_surface_example PRIVATE "/MANIFEST:NO")
  set_property(TARGET neui_surface_example APPEND PROPERTY
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/examples/neui_example.manifest")
elseif(WIN32)
  add_executable(neui_surface_example WIN32 examples/surface_example.cpp)
elseif(APPLE)
  add_executable(neui_surface_example MACOSX_BUNDLE examples/surface_example.cpp)
else()
  add_executable(neui_surface_example examples/surface_example.cpp)
endif()
target_link_libraries(neui_surface_example PRIVATE neui neui-xplhost)
if(WIN32)
  target_link_libraries(neui_surface_example PRIVATE neui-win32host)
endif()
if(APPLE)
  target_link_libraries(neui_surface_example PRIVATE neui-macoshost)
endif()

# Font-loading example. Registers a client-supplied font (NEUI_ASSET_KIND_FONT)
# via assets->create_font_from_file and proves the family reaches both the
# native-control prong (BUTTON / INPUTBOX) and the painted prong (CUSTOMDRAW
# push_font). Any .ttf dropped in examples/fonts/ is copied next to the
# executable so the bundled-resource path can be exercised; absent that the
# example falls back to a distinctive system font so it always renders.
if(MSVC)
  add_executable(neui_font_loading_example WIN32
    examples/font_loading_example.cpp
    examples/neui_example.rc)
  target_link_options(neui_font_loading_example PRIVATE "/MANIFEST:NO")
  set_property(TARGET neui_font_loading_example APPEND PROPERTY
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/examples/neui_example.manifest")
elseif(WIN32)
  add_executable(neui_font_loading_example WIN32 examples/font_loading_example.cpp)
elseif(APPLE)
  add_executable(neui_font_loading_example MACOSX_BUNDLE examples/font_loading_example.cpp)
else()
  add_executable(neui_font_loading_example examples/font_loading_example.cpp)
endif()
target_link_libraries(neui_font_loading_example PRIVATE neui neui-xplhost)
if(WIN32)
  target_link_libraries(neui_font_loading_example PRIVATE neui-win32host)
endif()
if(APPLE)
  target_link_libraries(neui_font_loading_example PRIVATE neui-macoshost)
endif()
# Copy any bundled fonts next to the executable (no-op if examples/fonts is
# empty / absent). On macOS bundles, place them in Resources/fonts.
file(GLOB NEUI_BUNDLED_FONTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/fonts/*.ttf"
                              "${CMAKE_CURRENT_SOURCE_DIR}/examples/fonts/*.otf"
                              "${CMAKE_CURRENT_SOURCE_DIR}/examples/fonts/*.ttc")
if(NEUI_BUNDLED_FONTS)
  if(APPLE)
    set_source_files_properties(${NEUI_BUNDLED_FONTS} PROPERTIES
      MACOSX_PACKAGE_LOCATION "Resources/fonts")
    target_sources(neui_font_loading_example PRIVATE ${NEUI_BUNDLED_FONTS})
  else()
    add_custom_command(TARGET neui_font_loading_example POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory
              "$<TARGET_FILE_DIR:neui_font_loading_example>/fonts"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${NEUI_BUNDLED_FONTS}
              "$<TARGET_FILE_DIR:neui_font_loading_example>/fonts")
  endif()
endif()

# Declarative "component" format example. Loads components/knob.json into a
# COMPONENT asset and instantiates four knobs from it (both create_from_component
# and create + set_asset), then re-serializes it (designer round-trip).
if(MSVC)
  add_executable(neui_component_knob_example WIN32
    examples/component_knob_example.cpp
    examples/neui_example.rc)
  target_link_options(neui_component_knob_example PRIVATE "/MANIFEST:NO")
  set_property(TARGET neui_component_knob_example APPEND PROPERTY
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/examples/neui_example.manifest")
elseif(WIN32)
  add_executable(neui_component_knob_example WIN32 examples/component_knob_example.cpp)
elseif(APPLE)
  add_executable(neui_component_knob_example MACOSX_BUNDLE examples/component_knob_example.cpp)
else()
  add_executable(neui_component_knob_example examples/component_knob_example.cpp)
endif()
target_link_libraries(neui_component_knob_example PRIVATE neui neui-xplhost)
if(WIN32)
  target_link_libraries(neui_component_knob_example PRIVATE neui-win32host)
endif()
if(APPLE)
  target_link_libraries(neui_component_knob_example PRIVATE neui-macoshost)
endif()
# Copy the component document(s) next to the executable so the relative
# "components/knob.json" path resolves at runtime. On macOS bundles, place them
# in Resources/components.
file(GLOB NEUI_COMPONENT_DOCS "${CMAKE_CURRENT_SOURCE_DIR}/examples/components/*.json")
if(NEUI_COMPONENT_DOCS)
  if(APPLE)
    set_source_files_properties(${NEUI_COMPONENT_DOCS} PROPERTIES
      MACOSX_PACKAGE_LOCATION "Resources/components")
    target_sources(neui_component_knob_example PRIVATE ${NEUI_COMPONENT_DOCS})
  else()
    add_custom_command(TARGET neui_component_knob_example POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory
              "$<TARGET_FILE_DIR:neui_component_knob_example>/components"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${NEUI_COMPONENT_DOCS}
              "$<TARGET_FILE_DIR:neui_component_knob_example>/components")
  endif()
endif()

# Scrolling SECTION example. Demonstrates NEUI_ATTR_SCROLL_MODE +
# NEUI_ATTR_ALIGN_TEXT="none" with runtime toggles for chip position,
# scroll axes, and a row-count-driven content generator. Pinned to the
# crossplatform host (where scrolling SECTION ships in v1).
if(MSVC)
  add_executable(neui_section_scroll_example WIN32
    examples/section_scroll_example.cpp
    examples/neui_example.rc)
  target_link_options(neui_section_scroll_example PRIVATE "/MANIFEST:NO")
  set_property(TARGET neui_section_scroll_example APPEND PROPERTY
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/examples/neui_example.manifest")
elseif(WIN32)
  add_executable(neui_section_scroll_example WIN32 examples/section_scroll_example.cpp)
elseif(APPLE)
  add_executable(neui_section_scroll_example MACOSX_BUNDLE examples/section_scroll_example.cpp)
else()
  add_executable(neui_section_scroll_example examples/section_scroll_example.cpp)
endif()
target_link_libraries(neui_section_scroll_example PRIVATE neui neui-xplhost)
if(WIN32)
  target_link_libraries(neui_section_scroll_example PRIVATE neui-win32host)
endif()
if(APPLE)
  target_link_libraries(neui_section_scroll_example PRIVATE neui-macoshost)
endif()

# Tabbed-view example: NEUI_W_TABVIEW / NEUI_W_TABPAGE - all 13 chip
# positions, scrolling page, per-chip colours, background + tab-outline
# border, and tab select/deselect events.
if(MSVC)
  add_executable(neui_tabview_example WIN32
    examples/tabview_example.cpp
    examples/neui_example.rc)
  target_link_options(neui_tabview_example PRIVATE "/MANIFEST:NO")
  set_property(TARGET neui_tabview_example APPEND PROPERTY
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/examples/neui_example.manifest")
elseif(WIN32)
  add_executable(neui_tabview_example WIN32 examples/tabview_example.cpp)
elseif(APPLE)
  add_executable(neui_tabview_example MACOSX_BUNDLE examples/tabview_example.cpp)
else()
  add_executable(neui_tabview_example examples/tabview_example.cpp)
endif()
target_link_libraries(neui_tabview_example PRIVATE neui neui-xplhost)
if(WIN32)
  target_link_libraries(neui_tabview_example PRIVATE neui-win32host)
endif()
if(APPLE)
  target_link_libraries(neui_tabview_example PRIVATE neui-macoshost)
endif()

# Minimal README example - input + button + label echo. Pure C; mirrors the
# code block in README.md verbatim. Useful as a smoke test that the public
# C API stays usable from a one-file client.
if(WIN32)
  add_executable(readme_example WIN32 examples/readme_example.c)
else()
  add_executable(readme_example examples/readme_example.c)
endif()
# Mixing a C-only main with neui's C++ static libs leaves MSVC's C++
# runtime + static initialisers unmounted, which manifests as an
# access violation inside the first create_session. Force the TU to
# compile as C++. The source is kept C99-clean so the snippet in the
# README still reads as C code.
set_source_files_properties(examples/readme_example.c PROPERTIES LANGUAGE CXX)
target_link_libraries(readme_example PRIVATE neui neui-xplhost)
if(WIN32)
  target_link_libraries(readme_example PRIVATE neui-win32host)
endif()
if(APPLE)
  target_link_libraries(readme_example PRIVATE neui-macoshost)
endif()

endif() # NEUI_BUILD_EXAMPLES

# Minimal iOS example app (XPL host). The desktop examples above are plain
# executables that can't bundle/launch on iOS, so iOS gets its own bundled
# target: a UIApplicationMain app whose SceneDelegate builds a tiny neui UI
# (SECTION + LABEL/BUTTON/INPUTBOX/CHECKBOX/SLIDER) after the scene connects.
# Pinned to the crossplatform host (the native iOS host is still a stub).
#
# To turn the built .app into something runnable in the simulator, build with
# code signing disabled (CODE_SIGNING_ALLOWED=NO / CODE_SIGNING_REQUIRED=NO),
# then `xcrun simctl install booted <path>.app` + `simctl launch`.
if(NEUI_BUILD_EXAMPLES AND NEUI_IOS)
  # A PNG bundled into the .app root so the IMAGE widget (and the
  # image_loader_ios.h bundle-resolution path) is exercised at runtime.
  set(NEUI_IOS_EXAMPLE_RES "${CMAKE_CURRENT_SOURCE_DIR}/examples/myimage.png")
  add_executable(neui_ios_example MACOSX_BUNDLE
    examples/ios/main.m
    examples/ios/AppDelegate.m
    examples/ios/SceneDelegate.mm
    ${NEUI_IOS_EXAMPLE_RES})
  # ARC for the ObjC / ObjC++ example sources (matches the host platform layer).
  set_source_files_properties(
    examples/ios/main.m
    examples/ios/AppDelegate.m
    examples/ios/SceneDelegate.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  # Copy the PNG into the bundle root (Resources flatten to the .app dir on iOS).
  # COMBINE_HIDPI_IMAGES off so a future @2x companion isn't merged into a .tiff,
  # which would break the path-keyed loader (same gotcha as the macOS example).
  set_source_files_properties(${NEUI_IOS_EXAMPLE_RES} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources"
    XCODE_ATTRIBUTE_COMBINE_HIDPI_IMAGES NO)
  set_target_properties(neui_ios_example PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/examples/ios/Info.plist"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.defiantnerd.neui.ios.example"
    MACOSX_BUNDLE_BUNDLE_NAME "neui iOS"
    XCODE_ATTRIBUTE_COMBINE_HIDPI_IMAGES NO
    # Target BOTH iPhone (1) and iPad (2). Without iPad in the device family the
    # app runs in iPhone-compatibility mode on iPad, so userInterfaceIdiom reports
    # Phone and the hardware-keyboard system menu bar machinery is never enabled
    # (-buildMenuWithBuilder: is iPad-only). "1,2" makes it a native iPad app so
    # the menu-bar contribution path is reachable; the hamburger still serves
    # iPhone.
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.defiantnerd.neui.ios.example")
  # neui-ioshost supplies neui_register_ioshost() (referenced by neui_init()
  # under NEUI_HAS_IOSHOST). It registers an empty native host stub today; the
  # example explicitly selects the crossplatform host, but the symbol must
  # still resolve - same pattern as the desktop examples linking neui-macoshost.
  target_link_libraries(neui_ios_example PRIVATE
    neui neui-xplhost neui-ioshost neui-backend-cg
    "-framework UIKit"
    "-framework Foundation"
    "-framework CoreGraphics"
    "-framework QuartzCore")
  # NEUIApplication (hosts/ios/application_ios.mm, in neui-ioshost) is the app's
  # principal class, but it is only referenced BY STRING in UIApplicationMain
  # (examples/ios/main.m), so the linker would dead-strip its object from the
  # static lib and -[UIApplicationMain] would fail to find the class. -force_load
  # the host lib so the ObjC class (and the app-level menu-bar contribution hooks)
  # are always present in the executable.
  target_link_options(neui_ios_example PRIVATE
    "-Wl,-force_load,$<TARGET_FILE:neui-ioshost>")
endif() # NEUI_IOS example
