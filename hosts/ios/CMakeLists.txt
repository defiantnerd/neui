cmake_minimum_required(VERSION 3.15)

# iOS-only native UIKit host (neui.host.ios). The top-level CMakeLists.txt
# gates this subdirectory on NEUI_IOS; defensive guard kept here for parity
# with hosts/macos/CMakeLists.txt's APPLE guard.
if(NOT NEUI_IOS)
  return()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MILESTONE 7: the native UIKit host. host.mm = Session + registration;
# window.mm = UIWindow / UIViewController / painted views + native control
# creation + paint; widgets.mm = the public API tables. Mirrors the
# hosts/macos/{host,window,widgets}.mm split.
add_library(neui-ioshost
  host.mm
  window.mm
  widgets.mm
  application_ios.mm)

set_source_files_properties(host.mm window.mm widgets.mm application_ios.mm PROPERTIES
  COMPILE_FLAGS "-fobjc-arc"
)

target_include_directories(neui-ioshost
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
          ${PROJECT_SOURCE_DIR}/hosts/shared
          ${PROJECT_SOURCE_DIR}/backends/cg
  PUBLIC  ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(neui-ioshost PRIVATE
  neui
  neui-backend-cg
  "-framework UIKit"
  "-framework Foundation"
  "-framework CoreGraphics"
  "-framework QuartzCore"
)
