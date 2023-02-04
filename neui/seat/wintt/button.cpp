#include "Button.h"

namespace wintt
{  

  Button::Button(HWND parent)
    : BaseWindow()
  {
    SubclassWindowProc(CreateWindowEx(WS_EX_LAYERED, _T("Button"), _T("Click Me!"), WS_CHILD | WS_VISIBLE | WS_TABSTOP, 20, 550, 340, 56, parent, (HMENU) 119, gInstance, nullptr));
    if (hwnd > 0)
    {
      // ::SetParent(hwnd, parent);
      ::SetLayeredWindowAttributes(hwnd, 0, 0, 0);
      SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOREDRAW | SWP_NOSIZE);
      setFont(18, _T("Segoe UI"));
    }
    else
    {
      auto out = ::GetLastError();
      fprintf(stderr,"err: %d\n", out);
      assert(false);
    }
    // this->setAlpha(70);
  }

  Button::~Button()
  {
  }


  LRESULT Button::handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam)
  {

    if (true && message == WM_RBUTTONDOWN) {
      WNDPROC wp = 0;
      HWND hParent = 0;
      setText("I was clicked!");
      // Button_SetText(hwnd, _T("I was clicked!"));
      POINT p{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
      //swprintf_s(buf, sizeof(buf), _T("Right-Click in control!\nx: %i\ny: %i\n\nNow, we'll convert this to the coordinates of the frame and pass the message up-stream!"),
      //  p.x, p.y);
      hParent = GetParent(hwnd);
      MessageBox(hParent, _T("Right Click, will bubble up..."), _T("Message"), MB_ICONINFORMATION);
      ClientToScreen(hwnd, &p);
      ScreenToClient(hParent, &p);
      SendMessage(hParent, WM_RBUTTONDOWN, wParam, MAKELPARAM(p.x, p.y));
    }
    return BaseWindow::handleWindowMessage(message, wParam, lParam);
  }
}