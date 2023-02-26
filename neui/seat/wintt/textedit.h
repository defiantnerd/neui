#pragma once
#include "controls.h"
namespace wintt
{
  class Label : public BaseWindow
  {
  public:
    Label(HWND parent, RECT pos);
    ~Label();
    LRESULT handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
  };

  class Textedit : public BaseWindow
  {
  public:
    Textedit(HWND parent, RECT pos);
    ~Textedit();
    LRESULT handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam) override;  
  };
}