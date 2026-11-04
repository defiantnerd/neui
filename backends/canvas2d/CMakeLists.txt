cmake_minimum_required(VERSION 3.15)

# HTML5 Canvas 2D backend for the crossplatform host on the web
# (plans/wasm-host.md). Mirrors backends/cairo on Linux. Its JavaScript half is
# embedded with EM_JS, so the library needs nothing at link time beyond what
# Emscripten provides.
if(NOT EMSCRIPTEN)
  return()
endif()

add_library(neui-backend-canvas2d canvas2d_backend.cpp)

target_include_directories(neui-backend-canvas2d
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  PUBLIC  ${PROJECT_SOURCE_DIR}/include
)
