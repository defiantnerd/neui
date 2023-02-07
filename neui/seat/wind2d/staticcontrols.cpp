#include "staticcontrols.h"
#include "common/events.h"
#include <dwmapi.h>

#pragma comment(lib, "dwmapi")

#define NEUI_WS_EX_LAYERED WS_EX_LAYERED

namespace neui
{
  namespace wind2d
  {
    using namespace win;

    Label::Label()
      : BaseWindow()
    {
      // this->setAlpha(70);
    }

    void Label::create()
    {
      HWND parent = getParentHWND();
      auto r = upscaleRect(rect);
      SubclassWindowProc(CreateWindowEx(NEUI_WS_EX_LAYERED | WS_EX_TRANSPARENT, _T("Static"),
        utf8_to_wstring(this->windowText).c_str(),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        r.x, r.y, r.w, r.h,
        parent, 0, gInstance, this));
      if (hwnd != NULL)
      {
        // ::SetParent(hwnd, parent);
        ::SetLayeredWindowAttributes(hwnd, 0, 0, 0);
        //SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOREDRAW | SWP_NOSIZE);
        // setFont(14, _T("Segoe UI"));
      }
      else
      {
        auto out = ::GetLastError();
        fprintf(stderr, "err: %d\n", out);
        assert(false);
      }
      super::create();
    }

#if 0
    LRESULT Label::handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
      //switch (message)
      //{
      //  case WM_ERASEBKGND:
      //    return 1;
      //    break;
      //  default:
      //    return BaseWindow::handleWindowMessage(message, wParam, lParam);
      //}
      return BaseWindow::handleWindowMessage(message, wParam, lParam);
    }
#endif

    Button::Button()
      : BaseWindow()
    {
      // this->setAlpha(70);
    }

    void Button::create()
    {
      HWND parent = NULL;
      assert(viewHandle.seat);
      if (parentView)
      {
        parent = NativeHandle(parentView->getNativeHandle());
      }
      auto r = upscaleRect(rect);
      SubclassWindowProc(CreateWindowEx(NEUI_WS_EX_LAYERED | 0, _T("Button"),
        utf8_to_wstring(this->windowText).c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_DEFPUSHBUTTON | BS_FLAT,
        r.x, r.y, r.w, r.h,
        parent, 0, gInstance, this));
      if (hwnd)
      {
        // ::SetParent(hwnd, parent);
        ::SetLayeredWindowAttributes(hwnd, 0, 0, 0);
        // SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOREDRAW | SWP_NOSIZE);
        // setFont(12, _T("Segoe UI"));
      }
      else
      {
        auto out = ::GetLastError();
        fprintf(stderr, "err: %d\n", out);
        assert(false);
      }
      super::create();
    }

    LRESULT Button::handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
      if (true && message == WM_LBUTTONUP) {
        WNDPROC wp = 0;
        HWND hParent = 0;
        // setText("I was clicked!", 0);
        POINT p{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        //swprintf_s(buf, sizeof(buf), _T("Right-Click in control!\nx: %i\ny: %i\n\nNow, we'll convert this to the coordinates of the frame and pass the message up-stream!"),
        //  p.x, p.y);
        event::Clicked ev(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), 0);
        viewHandle.sendEvent(ev);
#if 0
        // this is how to bubble up an event via Win32 HWNDs
        hParent = GetParent(hwnd);
        MessageBox(hParent, _T("Right Click, will bubble up..."), _T("Message"), MB_ICONINFORMATION);
        ClientToScreen(hwnd, &p);
        ScreenToClient(hParent, &p);
        SendMessage(hParent, WM_RBUTTONDOWN, wParam, MAKELPARAM(p.x, p.y));
#endif
      }
      return BaseWindow::handleWindowMessage(message, wParam, lParam);
    }

    bool TextField::setText(const std::string_view text, int32_t index)
    {
      if (index == 0)
      {
        label = text;
      }
      return super::setText(text, index);
    }

    void TextField::create()
    {
      HWND parent = NULL;
      assert(viewHandle.seat);
      if (parentView)
      {
        parent = NativeHandle(parentView->getNativeHandle());
      }
      auto r = upscaleRect(rect);
      SubclassWindowProc(CreateWindowEx(NEUI_WS_EX_LAYERED |
        WS_EX_CLIENTEDGE | WS_EX_LEFT | WS_EX_RIGHTSCROLLBAR, 
        _T("Edit"), utf8_to_wstring(this->label).c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP |  ES_AUTOHSCROLL,
        r.x, r.y, r.w, r.h,
        parent, 0, gInstance, this));
      if (hwnd)
      {
        // ::SetParent(hwnd, parent);
        ::SetLayeredWindowAttributes(hwnd, 0, 0, 0);
        // SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOREDRAW | SWP_NOSIZE);        
      }
      else
      {
        auto out = ::GetLastError();
        fprintf(stderr, "err: %d\n", out);
        assert(false);
      }

      super::create();

    }

    LRESULT TextField::handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
      // OutputDebugString(utf8_to_wstring( fmt::format("TEXT HWND {3:p}: MSG {0}, {1:x},{2:x}\n", message, wParam, lParam,(void*)hwnd)).c_str());
#if 0
      switch (message)
      {
        case WM_DPICHANGED:
        {
          currentDPI = HIWORD(wParam);
          // UpdateDpiDependentFontsAndResources();
          // WM_DPICHANGED comes with lParam pointing to a new rect for this control
          LPRECT r = (LPRECT)lParam;
          SetWindowPos(hwnd, hwnd, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        break;
        default:
          break;
      }
#endif
      switch (message)
      {
      case EN_CHANGE:
        OutputDebugStringA("oj");
        break;
      case WM_COMMAND:
        OutputDebugStringA("oj");
        break;
      }
      return BaseWindow::handleWindowMessage(message, wParam, lParam);
    }

    bool Droplist::setText(const std::string_view text, int32_t index)
    {
      if (index < 0)
      {
        this->text = text;
        super::setText(text, 0);
      }
      else
      {
        texts.push_back(std::string(text));
        if (hwnd)
        {
          auto k = ComboBox_AddString(hwnd, utf8_to_wstring(text).c_str());
        }

      }
      return true;
    }

    void Droplist::create()
    {

      HWND parent = NULL;
      assert(viewHandle.seat);
      if (parentView)
      {
        parent = NativeHandle(parentView->getNativeHandle());
      }
      auto r = upscaleRect(rect);
      SubclassWindowProc(CreateWindowEx(NEUI_WS_EX_LAYERED |
        WS_EX_CLIENTEDGE | WS_EX_LEFT | WS_EX_RIGHTSCROLLBAR,
        _T("ComboBox"), utf8_to_wstring(this->text).c_str(),
        CBS_DROPDOWN | CBS_HASSTRINGS | WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        r.x, r.y, r.w, r.h,
        parent, 0, gInstance, this));
      if (hwnd)
      {
        // ::SetParent(hwnd, parent);
        ::SetLayeredWindowAttributes(hwnd, 0, 0, 0);
        // SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOREDRAW | SWP_NOSIZE);        
      }
      else
      {
        auto out = ::GetLastError();
        fprintf(stderr, "err: %d\n", out);
        assert(false);
      }

      for (auto&& s : texts)
      {
        auto n = ComboBox_AddString(hwnd, utf8_to_wstring(s).c_str());
        OutputDebugStringA(fmt::format("string {} added with index {}", s, n).c_str());
      }
      super::setText(text, 0);

      super::create();
    }

    //LRESULT Droplist::handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam)
    //{
    //  return super::handleWindowMessage(message,wParam,lParam);
    //}

}
}