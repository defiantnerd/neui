cmake_minimum_required(VERSION 3.15)

# macOS-only native host. The top-level CMakeLists.txt already gates this
# subdirectory on APPLE; defensive guard kept here for parity with
# hosts/win32/CMakeLists.txt's WIN32 guard.
if(NOT APPLE)
  return()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(neui-macoshost
  host.mm
  widgets.mm
  window.mm)

set_source_files_properties(host.mm widgets.mm window.mm PROPERTIES
  COMPILE_FLAGS "-fobjc-arc"
)

target_include_directories(neui-macoshost
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
          ${PROJECT_SOURCE_DIR}/hosts/shared
  PUBLIC  ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(neui-macoshost PRIVATE
  neui
  neui-qrcodegen
  neui-backend-cg
  "-framework AppKit"
  "-framework Foundation"
  "-framework CoreGraphics"
  # UTType, for the NSOpenPanel / NSSavePanel type filters in
  # hosts/shared/macos/file_dialog_macos.h (macOS 11+; guarded at runtime).
  "-framework UniformTypeIdentifiers"
)
