#include "Button.h"

namespace wintt
{  

  Label::Label(HWND parent, RECT rect)
    : BaseWindow()
  {
    constexpr LONG additionalStyle = 0 
      //| WS_BORDER
      //| WS_VSCROLL | ES_LEFT 
      //|ES_MULTILINE | ES_AUTOVSCROLL
      ;
    static int i = 0;
    SubclassWindowProc(CreateWindowEx(WS_EX_LAYERED, _T("Static"), _T("Textlabel"),
      WS_CHILD | WS_VISIBLE | additionalStyle |  (i == 0 ? WS_GROUP : 0), 
      rect.left, rect.top, rect.right - rect.left + 1, rect.bottom - rect.top + 1, parent, (HMENU)120 + i, gInstance, nullptr));
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOREDRAW | SWP_NOSIZE);
    i++;
    if (hwnd > 0)
    {
      // ::SetParent(hwnd, parent);      
      // ::SetLayeredWindowAttributes(hwnd, 0, 0, 0);
      SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 255, LWA_ALPHA|LWA_COLORKEY);
      setFont(18, TEXT("Segoe UI"));
      // setAlpha(50);
    }
    else
    {
      auto out = ::GetLastError();
      fprintf(stderr, "err: %d\n", out);
    }
  }

  Label::~Label()
  {
    destroy();
  }


  LRESULT Label::handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam)
  {
    switch (message)
    {
    case WM_CTLCOLORSTATIC:
    {
      HDC hdcStatic = (HDC)wParam;
      SetBkMode(hdcStatic, TRANSPARENT);
      return 1;
      //SetTextColor(hdcStatic, RGB(255, 255, 255));
      //SetBkColor(hdcStatic, RGB(0, 0, 0));

      //if (hbrBkgnd == NULL)
      //{
      //  hbrBkgnd = CreateSolidBrush(RGB(0, 0, 0));
      //}
      //return (INT_PTR)hbrBkgnd;
    }
    case WM_ERASEBKGND:
      return 1;
      break;
      // we have to catch the message to allow this control to be used in a TABBED environment
      // to make it easier to use, it checks, if the style contains WS_TABSTOP
    case WM_GETDLGCODE:
    {
      bool hasWS_TABSTOP = (0 != (GetWindowLong(hwnd, GWL_STYLE) & WS_TABSTOP));
      if (hasWS_TABSTOP)
      {
        // THIS IS THE IMPORTANT PART
        // ***********************************
        LRESULT lres = CallWindowProc(BaseWindow::patchedWndProc, hwnd, message, wParam, lParam);
        lres &= ~DLGC_WANTTAB;
        if (lParam && ((MSG*)lParam)->message == WM_KEYDOWN && ((MSG*)lParam)->wParam == VK_TAB) {
          lres &= ~DLGC_WANTMESSAGE;
        }
        return lres;
        // ***********************************
      }
    }
    break;
    default:
      break;
    }

    if (false && message == WM_RBUTTONDOWN) {
      WNDPROC wp = 0;
      POINT p;
      HWND hParent = 0;
      p.x = LOWORD(lParam); // (lParam & 0x0000ffff);
      p.y = HIWORD(lParam); // (lParam >> 16);
      //swprintf_s(buf, sizeof(buf), _T("Right-Click in control!\nx: %i\ny: %i\n\nNow, we'll convert this to the coordinates of the frame and pass the message up-stream!"),
      //  p.x, p.y);
      hParent = GetParent(hwnd);
      MessageBox(GetParent(hParent), _T("Right Click"), _T("Message"), MB_ICONINFORMATION);
      ClientToScreen(hwnd, &p);
      ScreenToClient(hParent, &p);
      SendMessage(hParent, WM_RBUTTONDOWN, wParam, MAKELPARAM(p.x, p.y));
    }
    return BaseWindow::handleWindowMessage(message, wParam, lParam);
  }


  Textedit::Textedit(HWND parent, RECT rect)
    : BaseWindow()
  {
    constexpr LONG editstyles =
      WS_BORDER |
      WS_VSCROLL | ES_LEFT |
      ES_MULTILINE | ES_AUTOVSCROLL;
    static int i = 0;
    SubclassWindowProc(CreateWindowEx(WS_EX_LAYERED, _T("Edit"), _T("Textfeld"), WS_CHILD | WS_VISIBLE |  editstyles | WS_TABSTOP | (i==0 ? WS_GROUP : 0), rect.left, rect.top, rect.right-rect.left+1, rect.bottom-rect.top+1, parent, (HMENU) 120+i, gInstance, nullptr));
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOREDRAW | SWP_NOSIZE);
    i++;
    if (hwnd > 0)
    {
      // ::SetParent(hwnd, parent);      
      ::SetLayeredWindowAttributes(hwnd, 0, 0, 0);
      setFont(18, TEXT("Segoe UI"));
      // setAlpha(50);
    }
    else
    {
      auto out = ::GetLastError();
      fprintf(stderr,"err: %d\n", out);
    }
  }

  Textedit::~Textedit()
  {

  }


  LRESULT Textedit::handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam)
  {
    switch (message)
    {
      // we have to catch the message to allow this control to be used in a TABBED environment
      // to make it easier to use, it checks, if the style contains WS_TABSTOP
      case WM_GETDLGCODE:
      {
        bool hasWS_TABSTOP = (0 != (GetWindowLong(hwnd, GWL_STYLE) & WS_TABSTOP));
        if (hasWS_TABSTOP)
        {
          // THIS IS THE IMPORTANT PART
          // ***********************************
          LRESULT lres = CallWindowProc(BaseWindow::patchedWndProc, hwnd, message, wParam, lParam);
          lres &= ~DLGC_WANTTAB;
          if (lParam && ((MSG*)lParam)->message == WM_KEYDOWN && ((MSG*)lParam)->wParam == VK_TAB) {
            lres &= ~DLGC_WANTMESSAGE;
          }
          return lres;
          // ***********************************
        }
      }
      break;
    default:
      break;
    }

    if (false && message == WM_RBUTTONDOWN) {
      WNDPROC wp = 0;
      POINT p;
      HWND hParent = 0;
      p.x = LOWORD(lParam); // (lParam & 0x0000ffff);
      p.y = HIWORD(lParam); // (lParam >> 16);
      //swprintf_s(buf, sizeof(buf), _T("Right-Click in control!\nx: %i\ny: %i\n\nNow, we'll convert this to the coordinates of the frame and pass the message up-stream!"),
      //  p.x, p.y);
      hParent = GetParent(hwnd);
      MessageBox(GetParent(hParent), _T("Right Click"), _T("Message"), MB_ICONINFORMATION);
      ClientToScreen(hwnd, &p);
      ScreenToClient(hParent, &p);
      SendMessage(hParent, WM_RBUTTONDOWN, wParam, MAKELPARAM(p.x, p.y));
    }
    return BaseWindow::handleWindowMessage(message, wParam, lParam);
  }


}