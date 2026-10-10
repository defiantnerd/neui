# Shader phase 0 (plans/shader-effects.md): CPU executor prototype + benchmark.
#
# Works as a subdirectory of the neui build (registers the cross-compiler
# checksum test neui_shader_bench_check) and standalone, for quick timing runs
# without the rest of the tree:
#   cmake -S tests/shader_bench -B out/build-bench -DCMAKE_BUILD_TYPE=Release
#   cmake --build out/build-bench --config Release
#
# Phase 1 replaces this with src/shader_exec.cpp and moves the checksums into
# neui_tests.
cmake_minimum_required(VERSION 3.15)
if(NOT PROJECT_NAME)
  project(neui_shader_bench CXX)
endif()

# Shader arithmetic is always optimised and never FMA-contracted, in every
# build type: a Debug executor is 10-50x slower, and contraction changes bits.
# MSVC Debug carries /Od + /RTC1, which conflict with /O2; drop them for this
# directory only.
if(MSVC)
  string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
  string(REPLACE "/Od" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
endif()

add_executable(neui_shader_bench shader_bench.cpp)
target_compile_features(neui_shader_bench PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(neui_shader_bench PRIVATE /O2 /fp:precise)
else()
  target_compile_options(neui_shader_bench PRIVATE -O2 -ffp-contract=off)
  set_target_properties(neui_shader_bench PROPERTIES
    XCODE_ATTRIBUTE_GCC_OPTIMIZATION_LEVEL "2")
endif()

add_test(NAME neui_shader_bench_check COMMAND neui_shader_bench --check)
