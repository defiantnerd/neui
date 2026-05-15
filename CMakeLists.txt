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
elseif(WIN32)
  add_executable(neui_example WIN32 examples/main.cpp)
elseif(APPLE)
  # Bundle so AppKit can install the menubar / activate normally. The
  # example's `myimage.png` is copied into Contents/Resources so the
  # bundle works when launched from anywhere (Xcode, Finder, `open`),
  # not just from the project root via terminal where cwd matches.
  set(NEUI_EXAMPLE_RESOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/myimage.png")
  add_executable(neui_example MACOSX_BUNDLE
    examples/main.cpp
    ${NEUI_EXAMPLE_RESOURCES})
  set_source_files_properties(${NEUI_EXAMPLE_RESOURCES} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources")
else()
  add_executable(neui_example examples/main.cpp)
endif()

# Link the appropriate hosts. The win32 host is omitted on non-Windows;
# the xpl host is built everywhere.
target_link_libraries(neui_example PRIVATE neui neui-xplhost)
if(WIN32)
  target_link_libraries(neui_example PRIVATE neui-win32host)
endif()
if(APPLE)
  target_link_libraries(neui_example PRIVATE neui-macoshost)
endif()
