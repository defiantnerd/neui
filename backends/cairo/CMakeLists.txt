cmake_minimum_required(VERSION 3.15)

# Cairo software rendering backend for the crossplatform host on Linux.
# Mirrors backends/d2d on Windows and backends/cg on macOS.
if(WIN32 OR APPLE)
  return()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(CAIRO     REQUIRED cairo)
pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
find_package(Freetype REQUIRED)
find_package(X11 REQUIRED)

add_library(neui-backend-cairo cairo_backend.cpp)

# cairo_backend.h includes <X11/Xlib.h> and the .cpp blits via Xlib/XShm, so
# the X11 + rendering deps are PUBLIC: consumers that link only this static
# lib (e.g. the offscreen smoke test) inherit them at final link.
target_include_directories(neui-backend-cairo
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  PUBLIC  ${PROJECT_SOURCE_DIR}/include
          ${CAIRO_INCLUDE_DIRS}
          ${FONTCONFIG_INCLUDE_DIRS}
          ${X11_INCLUDE_DIR}
)

target_link_libraries(neui-backend-cairo PUBLIC
  ${CAIRO_LIBRARIES}
  ${FONTCONFIG_LIBRARIES}
  Freetype::Freetype
  X11::X11
  X11::Xext
)
