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
      {
        static WNDCLASSEX wcx;
        wcx.cbSize = sizeof(wcx);
        GetClassInfoEx(NULL, _T("STATIC"), &wcx);
        wcx.lpszClassName = _T("neui:STATIC");
        this->patchedWndProc = wcx.lpfnWndProc;
        wcx.lpfnWndProc = (WNDPROC)&BaseWindow::basicWndProc;
        wcx.hInstance = (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE);
        registerClass(wcx);
      }

      DWORD additionalStyles = 0;
      if (viewHandle.wantsEvent(event::Paint::type_v))
      {
        additionalStyles |= SS_OWNERDRAW;
      }
      auto r = upscaleRect(rect);
      CreateWindowEx(NEUI_WS_EX_LAYERED | WS_EX_TRANSPARENT,
        MAKEINTATOM(getClassAtom()),
        utf8_to_wstring(this->windowText).c_str(),
        WS_CHILD | WS_VISIBLE | SS_LEFT | additionalStyles,
        r.x, r.y, r.w, r.h,
        parent, 0, gInstance, this);
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

    void Button::create()
    {
      HWND parent = NULL;
      assert(viewHandle.seat);
      if (parentView)
      {
        parent = NativeHandle(parentView->getNativeHandle());
      }
      {
        static WNDCLASSEX wcx;
        wcx.cbSize = sizeof(wcx);
        GetClassInfoEx(NULL, _T("BUTTON"), &wcx);
        wcx.lpszClassName = _T("neui:BUTTON");
        this->patchedWndProc = wcx.lpfnWndProc;
        wcx.lpfnWndProc = (WNDPROC)&BaseWindow::basicWndProc;
        wcx.hInstance = (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE);
        registerClass(wcx);
      }

      auto r = upscaleRect(rect);
      CreateWindowEx(NEUI_WS_EX_LAYERED | 0,
        MAKEINTATOM(this->getClassAtom()),
        utf8_to_wstring(this->windowText).c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, //| BS_DEFPUSHBUTTON | BS_FLAT,
        r.x, r.y, r.w, r.h,
        parent, 0, gInstance, this);
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
      if (false && message == WM_LBUTTONUP) {
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


    void Checkbox::create()
    {
      HWND parent = NULL;
      assert(viewHandle.seat);
      if (parentView)
      {
        parent = NativeHandle(parentView->getNativeHandle());
      }
      {
        static WNDCLASSEX wcx;
        wcx.cbSize = sizeof(wcx);
        GetClassInfoEx(NULL, _T("BUTTON"), &wcx);
        wcx.lpszClassName = _T("neui:Checkbox");
        this->patchedWndProc = wcx.lpfnWndProc;
        wcx.lpfnWndProc = (WNDPROC)&BaseWindow::basicWndProc;
        wcx.hInstance = (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE);
        registerClass(wcx);
      }

      auto r = upscaleRect(rect);
      CreateWindowEx(NEUI_WS_EX_LAYERED | 0,
        MAKEINTATOM(getClassAtom()),
        utf8_to_wstring(this->windowText).c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_FLAT | BS_AUTOCHECKBOX,
        r.x, r.y, r.w, r.h,
        parent, 0, gInstance, this);
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
      Button_SetCheck(hwnd, (_checked) ? BST_CHECKED : BST_UNCHECKED);
      super::create();
    }

    LRESULT Checkbox::handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
      switch (message)
      {
      case UWM_BN_CLICKED:
      {
        BOOL checked = Button_GetCheck(hwnd);
        event::Selected ev(checked ? 1 : 0, 0);
        viewHandle.sendEvent(ev);
      }
      break;
      //case WM_COMMAND:
      //{
      //  auto result = BaseWindow::handleWindowMessage(message, wParam, lParam);
      //  if (wParam == BN_CLICKED)
      //  {
      //    BOOL checked = Button_GetCheck(hwnd);            
      //    event::Selected ev(!checked ? 1 : 0, 0);
      //    viewHandle.sendEvent(ev);
      //  }
      //  return result;
      //}
      break;
      default:
        break;
      }
      return BaseWindow::handleWindowMessage(message, wParam, lParam);
    }

    bool Checkbox::setInteger(const int32_t value, int32_t index)
    {
      if (index == 0 || index == -1)
      {
        _checked = (value != 0);
        if (hwnd)
        {
          Button_SetCheck(hwnd, (value != 0) ? BST_CHECKED : BST_UNCHECKED);
          if (index == 0)
          {
            event::Selected e(value, 0);
            this->viewHandle.sendEvent(e);
          }
          return true;
        }
      }
      return false;
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

      {
        static WNDCLASSEX wcx;
        wcx.cbSize = sizeof(wcx);
        GetClassInfoEx(NULL, _T("EDIT"), &wcx);
        wcx.lpszClassName = _T("neui:Edit");
        this->patchedWndProc = wcx.lpfnWndProc;
        wcx.lpfnWndProc = (WNDPROC)&BaseWindow::basicWndProc;
        wcx.hInstance = (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE);
        registerClass(wcx);
      }
      auto r = upscaleRect(rect);
      CreateWindowEx(NEUI_WS_EX_LAYERED |
        WS_EX_CLIENTEDGE | WS_EX_LEFT | WS_EX_RIGHTSCROLLBAR,
        MAKEINTATOM(getClassAtom()), utf8_to_wstring(this->label).c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        r.x, r.y, r.w, r.h,
        parent, 0, gInstance, this);
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

    bool Droplist::setText(const std::string_view text, int32_t index)
    {
      if (index == 0)
      {
        this->text = text;
        super::setText(text, 0);
      }
      else
      {
        auto listindex = index - 1;
        if (listindex < texts.size())
        {
          texts[listindex] = text;
        }
        else
        {
          texts.push_back(std::string(text));
        }
        if (hwnd)
        {
          auto n = ComboBox_GetCount(hwnd);
          if (listindex < n)
          {
            // item strings can not be replaced, just deleted/inserted
            ComboBox_DeleteString(hwnd, listindex);
            ComboBox_InsertString(hwnd, listindex, utf8_to_wstring(text).c_str());
          }
          else
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
      {
        static WNDCLASSEX wcx;
        wcx.cbSize = sizeof(wcx);
        GetClassInfoEx(NULL, _T("COMBOBOX"), &wcx);
        wcx.lpszClassName = _T("neui:COMBOBOX");
        this->patchedWndProc = wcx.lpfnWndProc;
        wcx.lpfnWndProc = (WNDPROC)&BaseWindow::basicWndProc;
        wcx.hInstance = (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE);
        registerClass(wcx);
      }

      auto r = upscaleRect(rect);
      CreateWindowEx(NEUI_WS_EX_LAYERED |
        WS_EX_CLIENTEDGE | WS_EX_LEFT | WS_EX_RIGHTSCROLLBAR,
        MAKEINTATOM(getClassAtom()), utf8_to_wstring(this->text).c_str(),
        CBS_DROPDOWN | CBS_HASSTRINGS | WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        r.x, r.y, r.w, r.h,
        parent, 0, gInstance, this);
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
      }
      super::setText(text, 0);

      super::create();
    }

    LRESULT Droplist::handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
      if (message == WM_COMMAND)
      {
        auto notificationcode = HIWORD(wParam);
        if (notificationcode == CBN_SELCHANGE)
        {
          auto k = ComboBox_GetCurSel(hwnd);
          viewHandle.eventcallback->setIntegerFromSeat(k, -1);
          event::Selected e(k, 0);
          this->viewHandle.sendEvent(e);
        }
      }
      return super::handleWindowMessage(message, wParam, lParam);
    }

    bool Droplist::setInteger(const int32_t value, int32_t index)
    {
      if (index == 0 || index == -1)
      {
        if (hwnd)
        {
          ComboBox_SetCurSel(hwnd, value);
          if (index == 0)
          {
            event::Selected e(value, 0);
            this->viewHandle.sendEvent(e);
          }
        }
      }
      return false;
    }

    void Slider::create() {

      HWND parent = NULL;
      assert(viewHandle.seat);
      if (parentView)
      {
        parent = NativeHandle(parentView->getNativeHandle());
      }
      {
        static WNDCLASSEX wcx;
        wcx.cbSize = sizeof(wcx);
        GetClassInfoEx(NULL, TRACKBAR_CLASS, &wcx);
        wcx.lpszClassName = _T("neui:TRACKBAR");
        this->patchedWndProc = wcx.lpfnWndProc;
        wcx.lpfnWndProc = (WNDPROC)&BaseWindow::basicWndProc;
        wcx.hInstance = (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE);
        registerClass(wcx);
      }

      auto r = upscaleRect(rect);
      CreateWindowEx( NEUI_WS_EX_LAYERED |
        WS_EX_LEFT | WS_EX_RIGHTSCROLLBAR,
        MAKEINTATOM(getClassAtom()), utf8_to_wstring(this->text).c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_OVERLAPPED
        | TBS_NOTICKS // | TBS_AUTOTICKS
        | TBS_BOTTOM | TBS_HORZ | TBS_BOTH
        | TBS_TRANSPARENTBKGND,
        r.x, r.y, r.w, r.h,
        parent, 0, gInstance, this);
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
      SendMessage(hwnd, TBM_SETRANGE,
        (WPARAM)TRUE,                   // redraw flag 
        (LPARAM)MAKELONG(0, 100));  // min. & max. positions

      SendMessage(hwnd, TBM_SETPAGESIZE,
        0, (LPARAM)10);                  // new page size 

      SendMessage(hwnd, TBM_SETSEL,
        (WPARAM)FALSE,                  // redraw flag 
        (LPARAM)MAKELONG(0, 100));

      SendMessage(hwnd, TBM_SETPOS,
        (WPARAM)TRUE,                   // redraw flag 
        (LPARAM)50);

      super::create();
    }
    LRESULT Slider::handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam) {
      return super::handleWindowMessage(message, wParam, lParam);
    }
    bool Slider::setInteger(const int32_t value, int32_t index)
    {
      return false;
    }

  }
}