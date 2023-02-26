#include "controls.h"

// declare the dependency for current common controls
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

HINSTANCE gInstance;

#if UNICODE
using tstring = std::wstring;
#else
using tstring = std::string;
#endif

namespace wintt
{
  int gDPI = 96;

  typedef LRESULT(*WndProc_t)(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

  LRESULT runLoop(HWND basehwnd)
  {
    MSG msg;
    // HACCEL hAccelTable = LoadAccelerators(gInstance, MAKEINTRESOURCE(IDC_WINTT));

    // Main message loop:
    while (GetMessage(&msg, nullptr, 0, 0))
    {
      // if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
      if (!IsDialogMessage(basehwnd, &msg))
      {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
      }
    }

    return (int)msg.wParam;
  }

  namespace util
  {
    void UpdateRectForDPI(HWND hWnd, LPRECT rect)
    {
      int iDpi = GetDpiForWindow(hWnd);
      int dpiScaledX = MulDiv(rect->left, iDpi, 96);
      int dpiScaledY = MulDiv(rect->top, iDpi, 96);
      int width = rect->right - rect->left;
      int height = rect->bottom - rect->top;
      int dpiScaledWidth = MulDiv(width, iDpi, 96);
      int dpiScaledHeight = MulDiv(height, iDpi, 96);
      rect->left = dpiScaledX;
      rect->top = dpiScaledY;
      rect->right = rect->left + dpiScaledWidth;
      rect->bottom = rect->top + dpiScaledHeight;
    }
  }

  BaseWindow::~BaseWindow()
  {
    if (hFont)
    {
      DeleteObject(hFont);
    }
    if (hwnd)
    {
      DestroyWindow(hwnd);
    }
    hwnd = 0;
    
  }

  auto BaseWindow::setAlpha(int percent) -> void
  {
    SetLayeredWindowAttributes(hwnd, 0, (255 * percent) / 100, LWA_ALPHA);
  }

  void BaseWindow::show(int show)
  {
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
  }

  bool BaseWindow::setText(const char* text)
  {
#if UNICODE
    std::string t(text);
    return setText(t);
#else
    return (0 == ::SetWindowText(hwnd, text));
#endif
  }

  bool BaseWindow::setText(const std::string& text)
  {
#if UNICODE
    auto wstr = utf8_to_wstring(text);
    return (0 == ::SetWindowText(hwnd, wstr.c_str()));
#else
    return (0 == ::setWindowText(hwnd, text.c_str()));
#endif

  }

  void BaseWindow::focus()
  {
    SetFocus(hwnd);
  }


  void BaseWindow::setFont(int size, const TCHAR* font)
  {
    if (hFont)
      DeleteObject(hFont);

    hFont = CreateFont(-size, 0, 0, 0, FW_DONTCARE, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
      CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH, font);

    SendMessage(hwnd,             // Handle of edit control
      WM_SETFONT,         // Message to change the font
      (WPARAM)hFont,     // handle of the font
      MAKELPARAM(TRUE, 0) // Redraw text
    );    
  }

  void BaseWindow::SubclassWindowProc(HWND hwnd)
  {
    this->hwnd = hwnd;
    this->patchedWndProc = (WNDPROC)GetWindowLongPtr(hwnd, GWLP_WNDPROC);
    SetWindowLongPtr(hwnd
      , GWLP_USERDATA,
      reinterpret_cast<LONG_PTR>(this));
    SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)BaseWindow::basicWndProc);
  }

  LRESULT BaseWindow::handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam)
  {
    switch (message)
    {
    case WM_DPICHANGED:
      {
      /*
        RECT rect;
        GetWindowRect(hwnd, &rect);
        SetWindowPos(hwnd, hwnd, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
        */
      gDPI = HIWORD(wParam);
      // UpdateDpiDependentFontsAndResources();
      // WM_DPICHANGED comes with lParam pointing to a new rect for this control
      LPRECT r = (LPRECT)lParam;
      SetWindowPos(hwnd, hwnd, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      }
      break;
    case WM_COMMAND:
      OutputDebugString(_T("ah, command!"));
      if (lParam)
      {
        auto win = (BaseWindow*)(GetWindowLongPtr((HWND)lParam, GWLP_USERDATA));
        if (win)
        {
          OutputDebugString(_T("ha!"));
        }
      }
      break;
    default:
      break;
    }
    // if the derived class subclassed a CommonControl window, the messages are passed to the
    // original WndProc of the control (which might finally end in DefWindowProc() as well).
    if (patchedWndProc)
    {
      return CallWindowProc(patchedWndProc, hwnd, message, wParam, lParam);
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
  }


  LRESULT CALLBACK BaseWindow::basicWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
  {
    // https://devblogs.microsoft.com/oldnewthing/20191014-00/?p=102992

    // checking if a class is 
    BaseWindow* self = (BaseWindow*)(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      LPCREATESTRUCT lpcs = reinterpret_cast<LPCREATESTRUCT>(lParam);
      self = static_cast<BaseWindow*>(lpcs->lpCreateParams);
      self->hwnd = hwnd;
      SetWindowLongPtr(hwnd
        , GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(self));
    }
    else {
      self = (BaseWindow*)(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }
    if (self)
    {
      if (self->getHWND() == hwnd) {

        return self->handleWindowMessage(message, wParam, lParam);
      }
      if (self->patchedWndProc)
      {
        return CallWindowProc(self->patchedWndProc, hwnd, message, wParam, lParam);
      }
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
  }

  // convert UTF-8 string to wstring
  std::wstring utf8_to_wstring(const std::string_view str)
  {
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
  }

  // convert wstring to UTF-8 string
  std::string wstring_to_utf8(const std::wstring& wstr)
  {

    BOOL usedDefault = false;
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, " ", &usedDefault);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, " ", &usedDefault);
    return strTo;
  }

  tstring LoadString(HINSTANCE hInstance, UINT uID)
  {
    PCTSTR pws;
    int cch = LoadString(hInstance, uID, reinterpret_cast<LPTSTR>(&pws), 0);
    return tstring(pws, cch);
  }

}