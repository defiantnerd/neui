cmake_minimum_required(VERSION 3.15)

# Vendored QR Code generator (Project Nayuki, MIT). Unlike stb_image.h this is
# not header-only - qrcodegen.cpp must be compiled - so it ships as a small
# static lib the host libraries link. The QR compound layer (hosts/shared/
# widget_paint_compound.h) calls qrcodegen::QrCode::encodeText to build the
# module matrix it rasterises into its internally-held bitmap.
add_library(neui-qrcodegen STATIC qrcodegen.cpp)

# Expose the header to consumers as a SYSTEM include so its own
# -Wconversion / -Wsign-conversion noise stays out of the host TUs that
# include it under the project's stricter /W4 -Wall -Wextra (and /WX in CI).
target_include_directories(neui-qrcodegen SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The third-party source itself inherits the project-wide /W4 -Wall -Wextra
# (and -Werror under NEUI_WERROR) from the top-level add_compile_options.
# Silence all warnings for this one TU we don't own. `/w` (lowercase) on MSVC
# disables warnings without the D9025 "overriding /W4" diagnostic that a second
# /W0 would emit; `-w` does the same on GCC / Clang and overrides -Werror.
if(MSVC)
  target_compile_options(neui-qrcodegen PRIVATE /w)
else()
  target_compile_options(neui-qrcodegen PRIVATE -w)
endif()
