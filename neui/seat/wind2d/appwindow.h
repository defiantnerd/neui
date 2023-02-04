#pragma once

#include "controls.h"
#include <string>

namespace neui
{

  namespace wind2d
  {
    using namespace std::string_literals;
    class AppWindow : public BaseWindow
    {
      using super = BaseWindow;
    public:
      AppWindow();
      ~AppWindow();
      void create() override;
      void destroy() override;
      bool setText(const std::string_view text, int32_t index) override;
      LRESULT handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
    private:
      void setIcon(const std::string_view iconname);
      HTHEME theme = 0;
      std::string windowtitle = "Application Window"s;
      std::string iconname;
      HICON hWindowIcon = NULL;
    };
  }
}