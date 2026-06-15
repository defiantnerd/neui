cmake_minimum_required(VERSION 3.15)

# CoreGraphics rendering backend for the crossplatform host on macOS.
# Mirrors backends/d2d on Windows and backends/null elsewhere.
if(NOT APPLE)
  return()
endif()

add_library(neui-backend-cg cg_backend.mm)

set_source_files_properties(cg_backend.mm PROPERTIES
  COMPILE_FLAGS "-fobjc-arc"
)

target_include_directories(neui-backend-cg
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  PUBLIC  ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(neui-backend-cg PRIVATE
  "-framework AppKit"
  "-framework CoreGraphics"
  "-framework CoreText"
  "-framework ImageIO"
  "-framework QuartzCore"
)
