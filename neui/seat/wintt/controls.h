#pragma once

#include "base.h"
#include "../Resource.h"

#include <vector>
#include <string>
#include <locale>

#include <cassert>
// include this and more 

// controls are included at the end of this file!

extern HINSTANCE gInstance;

namespace wintt
{

#if 0
  namespace internals
  {
    void addExStyle(HWND hwnd, unsigned long flags)
    {
      SetWindowLong(hwnd, GWL_EXSTYLE, GetWindowLong(hwnd, GWL_EXSTYLE) | flags);
    }

    void removeExStyle(HWND hwnd, unsigned long flags)
    {
      SetWindowLong(hwnd, GWL_EXSTYLE, GetWindowLong(hwnd, GWL_EXSTYLE) & ~flags);
    }

    class Registry
    {
      struct Entry
      {
        std::string classname;
        ATOM atom = 0;
        int32_t counter = 0;
      };
    public:
      ATOM registerClass(WNDCLASSEX wcex)
      {
#if UNICODE
        std::string classname(wstring_to_utf8(wcex.lpszClassName));
#else
        std::string classname(wcex.lpszClassName);
#endif
        auto pos = std::find_if(classes.begin(), classes.end(), [&](const Entry& item)
        {
          return item.classname == classname;
        });
        if (pos != classes.end())
        {
          // class name already registered
          pos->counter++;
          return pos->atom;
        }
        else
        {
          ATOM a = ::RegisterClassEx(&wcex);
          classes.emplace_back(Entry{ classname, a, 1 });
          return a;
        }
      }
      void unregisterClass(ATOM atom)
      {
#if UNICODE
        std::string classname(wstring_to_utf8(wcex.lpszClassName));
#else
        std::string classname(wcex.lpszClassName);
#endif
        auto pos = classes.find(classname);
      }
    private:
      std::vector<Entry> classes;
    };
  }
#endif

  class IWindow
  {
  public:
    virtual ~IWindow() {};
    virtual void setAlpha(int percent) = 0;
    virtual void show(int show = SW_SHOWDEFAULT) = 0;
    virtual void hide() = 0;
    virtual bool setText(const char* text) = 0;
    virtual bool setText(const std::string& text) = 0;
  };

  class FontStyle
  {
  public:
    // fontname
    // fontsize
    // fontattributes
  };
  class WidgetStyle
  {
  public:
    // pen color
    // paper color
    // text color
  };

  class BaseWindow : public IWindow
  {
  public:

    ~BaseWindow() override;
    void setAlpha(int percent) override;
    void show(int show = SW_SHOWDEFAULT) override;
    void hide() override { show(SW_HIDE); }
    bool setText(const char* text);
    bool setText(const std::string& text);
    void focus();
    HWND getHWND() const { return hwnd; }
    static LRESULT CALLBACK basicWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
  protected:
    BaseWindow() = default;
    void setFont(int size, const TCHAR* font);
    void SubclassWindowProc(HWND hwnd);
    virtual LRESULT handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam);
    HWND hwnd = 0;
    WNDPROC patchedWndProc = 0;
    HFONT hFont = 0;
  };

}

#include "mainwindow.h"
#include "frame.h"
#include "button.h"
#include "textedit.h"
#include "canvas.h"

