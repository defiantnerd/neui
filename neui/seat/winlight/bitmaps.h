#pragma once

#include "base.h"
#include <fmt/format.h>

namespace neui
{
  namespace d2d
  {
    class Context
    {
    public:
      Context(HWND hwnd, UINT w, UINT h);
      Context(HDC hdc, UINT w, UINT h);
      HRESULT createDeviceSpecificResources();
      void discardDeviceSpecificResources();
      HRESULT render();
      void resize(UINT width, UINT height);
    protected:
      ID2D1RenderTarget* getRenderTarget();
      ID2D1HwndRenderTarget* mRenderTargetHWND = nullptr;
      ID2D1DCRenderTarget* mRenderTargetDC = nullptr;
      HWND mHwnd = 0;
      HDC mDC = 0;
      D2D_SIZE_U mSize;
      // todo: remove later
      ID2D1SolidColorBrush* mLightSlateGrayBrush = nullptr;
      ID2D1SolidColorBrush* mCornflowerBlueBrush = nullptr;
    };
  }
}