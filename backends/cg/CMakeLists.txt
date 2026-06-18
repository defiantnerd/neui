cmake_minimum_required(VERSION 3.15)

# CoreGraphics rendering backend for the crossplatform host on macOS + iOS.
# Mirrors backends/d2d on Windows and backends/null elsewhere. The CG + CoreText
# draw code is shared; only the UI framework differs (AppKit on macOS, UIKit on
# iOS) - cg_backend.mm picks the import via TARGET_OS_IPHONE.
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

# AppKit only exists on macOS; on iOS the UI framework is UIKit.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(NEUI_CG_UI_FRAMEWORK "-framework UIKit")
else()
  set(NEUI_CG_UI_FRAMEWORK "-framework AppKit")
endif()

target_link_libraries(neui-backend-cg PRIVATE
  ${NEUI_CG_UI_FRAMEWORK}
  "-framework CoreGraphics"
  "-framework CoreText"
  "-framework ImageIO"
  "-framework QuartzCore"
)
