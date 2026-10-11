# neui-shader: the .nsl compiler + CPU executor (src/shader/nsl.h).
#
# The only place shader arithmetic is compiled. Every build type optimises it
# and never contracts a*b+c into an FMA, so every compiler and CPU produces the
# same bits (plans/shader-effects.md, "Phase 0 results"). MSVC Debug carries
# /Od + /RTC1, which conflict with /O2, and /Ob0, which turns off all inlining
# (the SIMD kernels run 6-11x slower); they are dropped for this directory only.
if(MSVC)
  string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
  string(REPLACE "/Od" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
  string(REPLACE "/Ob0" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
endif()

add_library(neui-shader STATIC nsl_compile.cpp nsl_exec.cpp nsl_gpu.cpp nsl_upscale.cpp)
target_compile_features(neui-shader PUBLIC cxx_std_17)
if(MSVC)
  target_compile_options(neui-shader PRIVATE /O2 /fp:precise)
else()
  target_compile_options(neui-shader PRIVATE -O2 -ffp-contract=off)
  set_target_properties(neui-shader PROPERTIES XCODE_ATTRIBUTE_GCC_OPTIMIZATION_LEVEL "2")
endif()
