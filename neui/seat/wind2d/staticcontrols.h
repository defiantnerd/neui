#pragma once

#include "controls.h"

namespace neui
{
  namespace wind2d
  {
    class Label : public BaseWindow
    {
      using super = BaseWindow;
    public:
      Label();
      void create() override;
    };

    class Button : public BaseWindow
    {
      using super = BaseWindow;
    public:
      Button() = default;
      void create() override;
      LRESULT handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
    };


    class Checkbox : public BaseWindow
    {
      using super = BaseWindow;
    public:
      Checkbox() = default;
      void create() override;
      LRESULT handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
      bool setInteger(const int32_t value, int32_t index) override;
    };

    class TextField : public BaseWindow
    {
      using super = BaseWindow;
    public:
      bool setText(const std::string_view text, int32_t index) override;
      void create() override;
      LRESULT handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
    private:
      std::string label;
    };

    class Droplist : public BaseWindow
    {
      using super = BaseWindow;
    public:
      bool setText(const std::string_view text, int32_t index) override;
      void create() override;
      LRESULT handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
      bool setInteger(const int32_t value, int32_t index) override;
    private:
      std::string text;
      std::vector<std::string> texts;
    };
  }
}