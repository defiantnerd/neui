// direct2d header file

#pragma once
#define NOMINMAX 1
// Windows Header Files:
#include <windows.h>
#include <memory>
#include "common/render.h"

namespace neui
{
  namespace gfx
  {
    namespace gdi
    {
      std::shared_ptr<IRenderer> make(HWND, const Rect);
    }

    namespace d2d
    {
      std::shared_ptr<IRenderer> make(HWND,const Rect);      
    }
  }
}