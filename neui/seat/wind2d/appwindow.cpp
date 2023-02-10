#include "appwindow.h"
#include <dwmapi.h>

#pragma comment(lib, "dwmapi")

namespace neui
{  
  namespace wind2d
  {
    using namespace win;

    AppWindow::AppWindow()
      : BaseWindow()
    {
    }

    void AppWindow::create()
    {
      WNDCLASSEX wcex;    // does not need to live beyond this function
      memset(&wcex, 0, sizeof(wcex));

      wcex.cbSize = sizeof(WNDCLASSEX);
      wcex.cbWndExtra = 0;
      wcex.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
      wcex.hCursor = LoadCursor(0, IDC_ARROW);
      wcex.lpfnWndProc = &BaseWindow::basicWndProc;
      wcex.lpszClassName = _T("neui::AppWindow");
      wcex.hInstance = gInstance;
      wcex.style = 0; //  CS_HREDRAW | CS_VREDRAW;

      registerClass(wcex);

      auto r = upscaleRect(rect);

      auto h = CreateWindowEx(WS_EX_APPWINDOW | WS_EX_LAYERED, MAKEINTATOM(getClassAtom()), 
        utf8_to_wstring(windowtitle).c_str(),
        WS_VISIBLE | WS_SYSMENU | WS_SIZEBOX | WS_OVERLAPPEDWINDOW, 
        r.x, r.y, r.w, r.h, 
        0, nullptr, gInstance, this);
      assert(h == hwnd);

      if (hwnd != 0)
      {
        ::SetLayeredWindowAttributes(hwnd, 0, 0, 0);
        setIcon(iconname);
      }
    }

    void AppWindow::destroy()
    {
      if (hwnd)
      {
        ::DestroyWindow(hwnd);
        hwnd = 0;
      }
    }

    bool AppWindow::setText(const std::string_view text, int32_t index)
    {
      switch (index)
      {
        case 0:
          windowtitle = text;
          break;
        case -1:
          iconname = text;
          setIcon(iconname);
          break;
        default:
          break;
      }
      return super::setText(text, index);
    }

    AppWindow::~AppWindow()
    {
      if (hWindowIcon)
      {
        ::DestroyIcon(hWindowIcon);
        hWindowIcon = NULL;
      }
    }

    LRESULT AppWindow::handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
      switch (message)
      {
        case WM_DWMCOLORIZATIONCOLORCHANGED:
          return BaseWindow::handleWindowMessage(message, wParam, lParam);
          break;
        case WM_CREATE:
          theme = GetWindowTheme(hwnd);
          resizeToDPI();
          return BaseWindow::handleWindowMessage(message, wParam, lParam);
          break;
        case WM_COMMAND:
        {
          int wmId = LOWORD(wParam);
          int eventId = HIWORD(wParam);
          // Parse the menu selections:
          switch (wmId)
          {
            case 0x29a0000:
              return 0;
              break;
            //case IDM_ABOUT:
            //  // DialogBox(gInstance, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
            //  break;
            //case IDM_EXIT:
            //  DestroyWindow(hwnd);
            //  break;
            default:
              return BaseWindow::handleWindowMessage(message, wParam, lParam);
          }
        }
        break;
        case WM_SIZE:
        {
          UINT width = LOWORD(lParam);
          UINT height = HIWORD(lParam);
          enumWin32ChildWindows(hwnd, [&](HWND window)->void
          {
            SendMessage(window, WMTT_PARENT_WM_SIZE, wParam, lParam);
          });
          auto result = BaseWindow::handleWindowMessage(message, wParam, lParam);
          return result;
        }
        break;
#if 0
        case WM_PAINT:
        {
          PAINTSTRUCT ps;
          HDC hdc = BeginPaint(hwnd, &ps);
          // TODO: Add any drawing code that uses hdc here...
          SetTextColor(hdc, RGB(20, 30, 40));
          SetBkMode(hdc, TRANSPARENT);
          // SetBkColor(hdc, RGB(20, 30, 40));
          TCHAR ptr[] = _T("Somewhere in the background, the HWND is there...");

          TextOut(hdc, 0, 0, ptr, ARRAYSIZE(ptr));
          EndPaint(hwnd, &ps);
        }
#endif
        break;

        case WM_DESTROY:
          PostQuitMessage(0);
          break;
        default:
          return BaseWindow::handleWindowMessage(message, wParam, lParam);
      }
      return 0;
    }

    void AppWindow::setIcon(const std::string_view iconname)
    {
      if (hwnd)
      {
        if (hWindowIcon)
        {
          ::DestroyIcon(hWindowIcon);
          hWindowIcon = NULL;
        }
        if (!iconname.empty())
        {
          // TODO: if the icon can not be loaded from the application, check the resource providers for a PNG
          hWindowIcon = ::LoadIcon(GetCurrentModule(), utf8_to_wstring(iconname).c_str());
          ::SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hWindowIcon);
          ::SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hWindowIcon);
        }
      }
    }

  }
}