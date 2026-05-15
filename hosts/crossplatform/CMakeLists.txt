cmake_minimum_required(VERSION 3.15)

add_library(neui-xplhost
  host.cpp
  widgets.cpp
  asset_manager.cpp
)

target_include_directories(neui-xplhost
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
          ${CMAKE_SOURCE_DIR}/hosts/shared
  PUBLIC  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(neui-xplhost PRIVATE neui)

if(WIN32)
  target_sources(neui-xplhost PRIVATE platform_win32.cpp)
  # windowsapp.lib supplies RoGetActivationFactory / RoOriginateLanguageException
  # required by the C++/WinRT projection used in theme_provider_win32.h.
  target_link_libraries(neui-xplhost PRIVATE neui-backend-d2d windowsapp)
elseif(APPLE)
  target_sources(neui-xplhost PRIVATE platform_macos.mm)
  set_source_files_properties(platform_macos.mm PROPERTIES
    COMPILE_FLAGS "-fobjc-arc"
  )
  target_link_libraries(neui-xplhost PRIVATE neui-backend-cg
    "-framework AppKit"
    "-framework Foundation"
    "-framework CoreGraphics"
  )
else()
  target_sources(neui-xplhost PRIVATE platform_null.cpp)
  target_link_libraries(neui-xplhost PRIVATE neui-backend-null)
endif()
