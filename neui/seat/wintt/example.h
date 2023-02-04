#pragma once
#include "controls.h"
namespace wintt
{
  class Example : public BaseWindow
  {
  public:
    Example(HWND parent);
    ~Example();
    LRESULT handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
  private:

  };
}