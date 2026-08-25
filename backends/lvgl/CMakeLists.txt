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

# Pinned to the exact upstream commit this backend was written and measured
# against. LVGL master churns the very APIs lvgl_backend.cpp uses (the
# lv_draw_vector_dsc_* naming, the lv_obj flag setters that already log
# LV_LOG_DEPRECATED), so tracking `master` meant a fresh configure could fail to
# compile for reasons unrelated to any neui change - and, because CMake caches
# the clone, an existing tree would silently keep building an older revision.
# Bump deliberately, re-running the measurements in plans/lvgl-host-approach-c.md.
#
# GIT_SHALLOW is off: a shallow clone checks out a branch tip, which cannot
# resolve a bare commit hash. The full clone is a one-time configure cost on a
# build that goes on to compile all of LVGL + ThorVG anyway.
FetchContent_Declare(lvgl
  GIT_REPOSITORY https://github.com/lvgl/lvgl.git
  GIT_TAG        066d8db0b54819223357731f68961a90b3d785b4  # master, 2026-07-30
)

# neui compiles at /W4 (its warnings-as-linter policy), but that directory
# property would also apply to the fetched LVGL + ThorVG sources and drown
# the build in third-party noise. Drop the high-warning flags from this
# directory's inherited options and re-apply them to neui's own targets below.
# This MUST cover the NEUI_WERROR flags too: leaving /WX (or -Werror) on the
# directory turns the first third-party warning in LVGL or ThorVG into a hard
# build failure that no neui change can fix - the exact outcome this scrub
# exists to prevent. Keep in sync with the top-level CMakeLists.
get_directory_property(_neui_dir_opts COMPILE_OPTIONS)
set(_neui_own_warnings "")
if(MSVC)
  set(_neui_own_warnings /W4 /wd4100)
  if(NEUI_WERROR)
    list(APPEND _neui_own_warnings /WX /wd4996)
  endif()
else()
  set(_neui_own_warnings -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
  if(NEUI_WERROR)
    list(APPEND _neui_own_warnings -Werror -Wno-error=deprecated-declarations)
  endif()
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
