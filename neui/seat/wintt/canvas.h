#pragma once
#include "controls.h"
#include "direct2d.h"
#include "apihelper.h"

namespace wintt
{
  class Canvas : public BaseWindow
  {
  public:
    Canvas(HWND parent);
    ~Canvas();
    LRESULT handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
  protected:
    WindowClassHandle classHandle;
    std::unique_ptr<d2d::Context> context;
  };

  class Direct2DBaseWindow : public BaseWindow
  {
  public:
    ~Direct2DBaseWindow();
    LRESULT handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
  protected:
    WindowClassHandle classHandle;
    virtual void render(ID2D1RenderTarget* rendertarget) = 0;
    Direct2DBaseWindow(HWND parent);
    std::unique_ptr<d2d::Context> context;
  };
}