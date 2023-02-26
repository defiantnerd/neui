#pragma once
#include "controls.h"
namespace wintt
{
  class MainWindow : public BaseWindow
  {
  public:
    MainWindow();
    ~MainWindow();
    LRESULT handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
  private:
    HTHEME theme = 0;
  };
}