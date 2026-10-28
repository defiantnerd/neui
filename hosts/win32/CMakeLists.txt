cmake_minimum_required(VERSION 3.15)
project(neui LANGUAGES C CXX)

# Win32-only host. The top-level CMakeLists.txt already gates this
# subdirectory on WIN32, but a defensive guard here keeps things sane
# if someone wires it up directly.
if(NOT WIN32)
  return()
endif()

# Set C++ standard to C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# build a library with a appropriate host
# winmain.cpp is the application's entry point, an object of its own: the linker
# takes it only where the C runtime asks for WinMain (an application), never into
# a DLL such as an audio plugin embedding a frame (NEUI_API_EMBED).
add_library(neui-win32host host.cpp widgets.cpp window.cpp winmain.cpp)

target_include_directories(neui-win32host
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(neui-win32host PRIVATE neui neui-qrcodegen neui-backend-d2d
                                            windowsapp dwmapi)
