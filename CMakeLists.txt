cmake_minimum_required(VERSION 3.15)
project(neui LANGUAGES C CXX)

# Objective-C++ is needed for the macOS host platform layer (.mm files).
if(APPLE)
  enable_language(OBJCXX)
endif()

# Set C++ standard to C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# build a library with a appropriate host
add_library(neui src/neui.c)

# Per-platform host + backend selection.
#   Windows : neui-win32host (native HWND host) + neui-xplhost (D2D-rendered)
#   macOS   : neui-xplhost (CoreGraphics-rendered) - neui-win32host is omitted
#   Other   : neui-xplhost (null backend)          - same
if(WIN32)
  add_subdirectory("hosts/win32")
  add_subdirectory("backends/d2d")
endif()
if(APPLE)
  add_subdirectory("backends/cg")
  add_subdirectory("hosts/macos")
endif()
add_subdirectory("backends/null")
add_subdirectory("hosts/crossplatform")

# Tier 1 unit tests for the portable shared logic (hosts/shared/*.h). They
# depend only on the public headers + the shared utilities, so they build
# everywhere. Toggle with -DNEUI_BUILD_TESTS=OFF.
option(NEUI_BUILD_TESTS "Build the neui unit tests" ON)
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
if(APPLE)
  target_compile_definitions(neui PRIVATE NEUI_HAS_MACOSHOST)
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
