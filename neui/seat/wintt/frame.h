#pragma once
#include "controls.h"
namespace wintt
{
  class Frame : public BaseWindow
  {
  public:
    Frame(HWND parent);
    ~Frame();
    LRESULT handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
  private:

  };
}