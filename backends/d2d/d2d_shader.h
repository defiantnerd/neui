#pragma once

// GPU shader programs for the D2D backend (renderer.h create_shader_program /
// destroy_shader_program / draw_shader_program): HLSL compiled at runtime by
// the OS's d3dcompiler_47.dll and run by one zero-input custom effect.
// d2d_backend.cpp owns the device chain and the contexts; this file owns the
// effect class, the compiled programs and their per-device-context effects.

#include <windows.h>
#include <d2d1_1.h>
#include <d3dcommon.h>

#include <cstddef>
#include <cstdint>

namespace neui_d2d_backend
{
  namespace shader
  {
    // Starts compiling `src` (NEUI_SHADER_LANG_HLSL_D2D) for a device of
    // feature level `fl` on a worker thread. Null without the compiler or
    // below feature level 10_0. draw() returns false until the compile is
    // done, and for good after a compile error (which goes to
    // OutputDebugString).
    void* create_program(ID2D1Factory1* factory, D3D_FEATURE_LEVEL fl, int lang,
                         const char* src, size_t len);
    void  destroy_program(void* program);

    // Draws `program` at device pixel (x, y), w x h px, into `dc` (inside
    // BeginDraw), with `opacity` multiplied into the constants' opacity slot.
    bool  draw(ID2D1DeviceContext* dc, float opacity, void* program,
               int32_t x, int32_t y, uint32_t w, uint32_t h,
               const float* constants, uint32_t count);

    // A device context is going away (destroy or device loss): drop the
    // effects made on it.
    void  release_dc(ID2D1DeviceContext* dc);
  }
}
