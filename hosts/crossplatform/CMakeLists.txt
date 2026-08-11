cmake_minimum_required(VERSION 3.15)

add_library(neui-xplhost
  host.cpp
  widgets.cpp
  asset_manager.cpp
  a11y_adapter.cpp
)

target_include_directories(neui-xplhost
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
          ${PROJECT_SOURCE_DIR}/hosts/shared
  PUBLIC  ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(neui-xplhost PRIVATE neui neui-qrcodegen)

if(WIN32)
  target_sources(neui-xplhost PRIVATE platform_win32.cpp)
  # windowsapp.lib supplies RoGetActivationFactory / RoOriginateLanguageException
  # required by the C++/WinRT projection used in theme_provider_win32.h.
  # ole32 / shell32: CoCreateInstance + SHCreateItemFromParsingName for the
  # IFileDialog file dialogs (file_dialog_win32.h).
  target_link_libraries(neui-xplhost PRIVATE neui-backend-d2d windowsapp
                                             ole32 shell32)
elseif(NEUI_IOS)
  # iOS uses the same shared CoreGraphics backend as macOS, but the UIKit
  # platform layer instead of the AppKit one. This branch MUST come before the
  # generic APPLE branch since iOS also matches APPLE.
  target_sources(neui-xplhost PRIVATE platform_ios.mm)
  set_source_files_properties(platform_ios.mm PROPERTIES
    COMPILE_FLAGS "-fobjc-arc"
  )
  target_link_libraries(neui-xplhost PRIVATE neui-backend-cg
    "-framework UIKit"
    "-framework Foundation"
    "-framework CoreGraphics"
    "-framework QuartzCore"
  )
elseif(APPLE)
  target_sources(neui-xplhost PRIVATE platform_macos.mm)
  set_source_files_properties(platform_macos.mm PROPERTIES
    COMPILE_FLAGS "-fobjc-arc"
  )
  target_link_libraries(neui-xplhost PRIVATE neui-backend-cg
    "-framework AppKit"
    "-framework Foundation"
    "-framework CoreGraphics"
    # UTType, for the NSOpenPanel / NSSavePanel type filters in
    # hosts/shared/macos/file_dialog_macos.h (macOS 11+; guarded at runtime).
    "-framework UniformTypeIdentifiers"
  )
elseif(UNIX)
  # Linux (and other X11 Unixes): Cairo software backend + X11 platform layer.
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(CAIRO      REQUIRED cairo)
  pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
  find_package(X11 REQUIRED)
  find_package(Freetype REQUIRED)

  target_sources(neui-xplhost PRIVATE platform_linux.cpp)
  # Third-party headers (vendored stb + Cairo/X11 system libs) are marked
  # SYSTEM so their own -Wconversion / -Wsign-conversion noise stays out of
  # consumer builds that compile neui under stricter flags than neui itself
  # uses. stb_image.h in particular emits hundreds of conversion warnings.
  target_include_directories(neui-xplhost SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/stb
    ${CAIRO_INCLUDE_DIRS}
    ${X11_INCLUDE_DIR})
  target_link_libraries(neui-xplhost PRIVATE
    neui-backend-cairo
    X11::X11
    X11::Xext            # XShm
    ${CAIRO_LIBRARIES}
    ${FONTCONFIG_LIBRARIES}
    Freetype::Freetype)

  # XInput2 smooth scroll is an optional enhancement: if libXi (dev) is present
  # the host mines XI2 scroll valuators for pixel-precise / kinetic scrolling;
  # otherwise it falls back to classic core Button 4-7 stepped scrolling. Build
  # never hard-depends on libXi-dev.
  pkg_check_modules(XI xi)
  if(XI_FOUND)
    target_compile_definitions(neui-xplhost PRIVATE NEUI_HAS_XI2=1)
    target_include_directories(neui-xplhost PRIVATE ${XI_INCLUDE_DIRS})
    target_link_libraries(neui-xplhost PRIVATE ${XI_LIBRARIES})
    message(STATUS "neui: XInput2 smooth scroll enabled (libXi found)")
  else()
    message(STATUS "neui: libXi not found - XInput2 smooth scroll disabled "
                   "(core Button 4-7 stepped scroll fallback). Install libxi-dev to enable.")
  endif()

  # Optional: system dark/light tracking via the XDG desktop portal over D-Bus.
  # Present -> NEUI_ATTR_THEME_MODE=AUTO follows the system color-scheme live;
  # absent -> the host keeps its default palette (no system tracking).
  pkg_check_modules(DBUS dbus-1)
  if(DBUS_FOUND)
    target_compile_definitions(neui-xplhost PRIVATE NEUI_HAS_DBUS=1)
    target_include_directories(neui-xplhost PRIVATE ${DBUS_INCLUDE_DIRS})
    target_link_libraries(neui-xplhost PRIVATE ${DBUS_LIBRARIES})
    message(STATUS "neui: system dark-mode tracking enabled (libdbus-1 found)")
  else()
    message(STATUS "neui: libdbus-1 not found - system dark-mode tracking disabled. "
                   "Install libdbus-1-dev to enable.")
  endif()
else()
  target_sources(neui-xplhost PRIVATE platform_null.cpp)
  target_link_libraries(neui-xplhost PRIVATE neui-backend-null)
endif()
