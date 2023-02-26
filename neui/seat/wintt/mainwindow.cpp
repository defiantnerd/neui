#include "mainwindow.h"
#include <dwmapi.h>

#pragma comment(lib, "dwmapi")

namespace wintt
{
  MainWindow::MainWindow()
    : BaseWindow()
  {
    
    WNDCLASSEX wcex;
    memset(&wcex, 0, sizeof(wcex));

    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.cbWndExtra = 0;
    wcex.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wcex.hCursor = LoadCursor(0, IDC_ARROW);
    wcex.lpfnWndProc = &BaseWindow::basicWndProc;
    wcex.lpszClassName = _T("MainWindow");
    wcex.hInstance = gInstance;
    wcex.style = 0; //  CS_HREDRAW | CS_VREDRAW;

    RegisterClassEx(&wcex);
    // WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr
    
    // SubclassWindowProc(CreateWindowEx(WS_EX_APPWINDOW | WS_EX_LAYERED, /* MAKEINTATOM(classAtom) */_T("MainWindow"), _T("My Window"), WS_VISIBLE | WS_SYSMENU | WS_SIZEBOX | WS_OVERLAPPEDWINDOW, 100, 100, 1000, 1000, 0, nullptr, gInstance, this));
    auto h = CreateWindowEx(WS_EX_APPWINDOW | WS_EX_LAYERED, /* MAKEINTATOM(classAtom) */_T("MainWindow"), _T("My Window"), WS_VISIBLE | WS_SYSMENU | WS_SIZEBOX | WS_OVERLAPPEDWINDOW, 100, 100, 1000, 1000, 0, nullptr, gInstance, this);
    assert(h == hwnd);

    if (hwnd > 0)
    {     
      ::SetLayeredWindowAttributes(hwnd, 0, 0, 0);


    }
    // this->setAlpha(70);
  }

  MainWindow::~MainWindow()
  {
    if (hwnd)
    {
      ::DestroyWindow(hwnd);
      hwnd = 0;
    }
    UnregisterClass(_T("MainWindow"), gInstance);
  }

  LRESULT MainWindow::handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam)
  {
    switch (message)
    {
    //case WM_DWMCOLORIZATIONCOLORCHANGED:
    //  return BaseWindow::handleWindowMessage(message, wParam, lParam);
    //  break;
    case WM_CREATE:
      theme = GetWindowTheme(hwnd);
      return BaseWindow::handleWindowMessage(message, wParam, lParam);
      break;
    case WM_COMMAND:
    {
      int wmId = LOWORD(wParam);
      int eventId = HIWORD(wParam);
      // Parse the menu selections:
      switch (wmId)
      {
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
    break;
    case WM_DESTROY:
      PostQuitMessage(0);
      break;
    default:
      return BaseWindow::handleWindowMessage(message, wParam, lParam);
    }
    return 0;
  }

}