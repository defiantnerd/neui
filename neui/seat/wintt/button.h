#pragma once
#include "controls.h"
namespace wintt
{
  class Button : public BaseWindow
  {
  public:
    Button(HWND parent);
    ~Button();
    LRESULT handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam) override;  
  };
}