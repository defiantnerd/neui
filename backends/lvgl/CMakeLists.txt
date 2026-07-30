cmake_minimum_required(VERSION 3.15)

# LVGL rendering backend for the crossplatform host (prototype - see
# plans/lvgl-host-approach-c.md). Windows-only for now; the top-level
# CMakeLists gates this subdirectory on NEUI_WITH_LVGL and fails hard on
# unsupported platforms.
if(NOT NEUI_WITH_LVGL)
  return()
endif()

include(FetchContent)

# LVGL configuration: repo-local override-style lv_conf.h, generated from
# lv_conf.h.in so the framebuffer depth is a configure-time choice
# (-DNEUI_LVGL_COLOR_DEPTH=32 for XRGB8888, =16 for RGB565). No LVGL demos or
# examples compiled into the build.
set(NEUI_LVGL_COLOR_DEPTH 32 CACHE STRING
    "LV_COLOR_DEPTH for the LVGL host build (32 = XRGB8888, 16 = RGB565)")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/lv_conf.h" @ONLY)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_BINARY_DIR}/lv_conf.h" CACHE PATH
    "neui: LVGL configuration header" FORCE)
set(CONFIG_LV_BUILD_DEMOS    OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# Fetched from git main per the prototype plan. Version pin is a deliberate
# follow-up before this leaves prototype status (note in the plan).
FetchContent_Declare(lvgl
  GIT_REPOSITORY https://github.com/lvgl/lvgl.git
  GIT_TAG        master
  GIT_SHALLOW    TRUE
)

# neui compiles at /W4 (its warnings-as-linter policy), but that directory
# property would also apply to the fetched LVGL + ThorVG sources and drown
# the build in third-party noise. Drop the high-warning flags from this
# directory's inherited options and re-apply them to neui's own targets below.
get_directory_property(_neui_dir_opts COMPILE_OPTIONS)
set(_neui_own_warnings "")
if(MSVC)
  set(_neui_own_warnings /W4 /wd4100)
else()
  set(_neui_own_warnings -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()
foreach(_flag IN LISTS _neui_own_warnings)
  list(REMOVE_ITEM _neui_dir_opts "${_flag}")
endforeach()
set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_neui_dir_opts}")

FetchContent_MakeAvailable(lvgl)

# The rendering backend proper: neui_render_backend_t over LVGL draw tasks.
# lvgl is a PUBLIC dependency so consumers (the xpl host's platform_lvgl.cpp)
# inherit the include dirs + LV_CONF_PATH definition.
add_library(neui-backend-lvgl lvgl_backend.cpp)
target_compile_options(neui-backend-lvgl PRIVATE ${_neui_own_warnings})
target_include_directories(neui-backend-lvgl
  PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(neui-backend-lvgl PUBLIC lvgl)
