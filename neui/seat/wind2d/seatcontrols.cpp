#include "seatcontrols.h"
#include "base.h"
// #include <fmt/format.h>

HINSTANCE gInstance = 0;

namespace neui
{
  using namespace win;

#if UNICODE
  using tstring = std::wstring;
#else
  using tstring = std::string;
#endif


  namespace wind2d
  {

    class ClassRegistry;

    static std::shared_ptr<ClassRegistry> classRegistry;

    class ClassInstanceImpl : public ClassInstance
    {
    public:
      ClassInstanceImpl(std::shared_ptr<ClassRegistry>& registry, ATOM atom)
        : registry(registry)
        , atom(atom)
      {}
      ATOM getAtom() const override { return atom; };
      ~ClassInstanceImpl();
    private:
      std::shared_ptr<ClassRegistry> registry;
      ATOM atom;
    };

    struct ClassRegistration
    {
      int32_t refCounter = 0;
      ATOM atom = 0;
      tstring classname;
    };

    class ClassRegistry
    {
    public:
      static std::shared_ptr<ClassInstance> registerClass(WNDCLASSEX& wc)
      {
        if (!classRegistry)
        {
          classRegistry = std::make_shared<ClassRegistry>();
        }
        for (auto& r : classRegistry->registry)
        {
          if (r.classname.compare(wc.lpszClassName) == 0)
          {
            r.refCounter++;
            return std::make_shared<ClassInstanceImpl>(classRegistry, r.atom);
          }
        }
        auto atom = ::RegisterClassEx(&wc);
        classRegistry->registry.push_back({ 1,atom,wc.lpszClassName });
        return std::make_shared<ClassInstanceImpl>(classRegistry, atom);
      }

      void unregisterClass(ATOM atom)
      {
        for (auto r = registry.begin(); r != registry.end(); ++r)
        {
          if (r->atom == atom)
          {
            if (--r->refCounter == 0)
            {
              registry.erase(r);
              ::UnregisterClass(MAKEINTATOM(atom), gInstance);
              return;
            }
            break;
          }
        }
      }
      ~ClassRegistry()
      {
        OutputDebugString(_T("Destruct Registry\n"));
      }
    private:
      std::list<ClassRegistration> registry;
    };

    ClassInstanceImpl::~ClassInstanceImpl()
    {
      registry->unregisterClass(atom);
    }


    extern HFONT gDefaultFont;

    void VirtualWidget::setViewHandle(const ViewHandle& handle)
    {
      viewHandle = handle;
    }

    void VirtualWidget::setRect(const Rect& r)
    {
      rect = r;
    }

    void VirtualWidget::setBoxModel(const BoxModel& bm)
    {
      boxmodel = bm;
    }

    BaseWindow::~BaseWindow()
    {
      destroy();
      if (hFont)
      {
        ::DeleteObject(hFont);
      }
    }

    void BaseWindow::registerClass(WNDCLASSEX& wc)
    {
      classInstance = ClassRegistry::registerClass(wc);
    }

    auto BaseWindow::setAlpha(int percent) -> void
    {
      ::SetLayeredWindowAttributes(hwnd, 0, (255 * percent) / 100, LWA_ALPHA);
    }

    float BaseWindow::getDpi()
    {
      return (float)currentDPI;
    }

    RECT BaseWindow::upscaleRECT(const Rect& r)
    {
      // auto factor = MulDiv(r.x, dpiFactor,96);
      auto left = MulDiv(r.x, currentDPI, 96);
      auto top = MulDiv(r.y, currentDPI, 96);
      auto right = MulDiv(r.w, currentDPI, 96) + left;
      auto bottom = MulDiv(r.h, currentDPI, 96) + top;
      return { left, top, right, bottom };
    }

    Rect BaseWindow::upscaleRect(const Rect& r)
    {
      auto x = MulDiv(r.x, currentDPI, 96);
      auto y = MulDiv(r.y, currentDPI, 96);
      auto w = MulDiv(r.w, currentDPI, 96);
      auto h = MulDiv(r.h, currentDPI, 96);
      return { x,y,w,h };
    }

    void BaseWindow::create()
    {
      if (hwnd)
      {
        currentDPI = GetWindowDPI(hwnd);
        setDefaultFont();
        resizeToDPI();
      }
    }

    void BaseWindow::destroy()
    {
      if (hwnd)
      {
        ::DestroyWindow(hwnd);
        hwnd = 0;
      }
      classInstance.reset();
    }

    void BaseWindow::show(int show)
    {
      ::ShowWindow(hwnd, show);
      ::UpdateWindow(hwnd);
    }

    void BaseWindow::focus()
    {
      ::SetFocus(hwnd);
    }

    //    bool BaseWindow::setText(const char* text)
    //    {
    //#if UNICODE
    //      std::string t(text);
    //      return setText(t);
    //#else
    //      return (0 == ::SetWindowText(hwnd, text));
    //#endif
    //    }

    bool BaseWindow::setText(const std::string_view text, int32_t index)
    {
      if (index == 0)
      {
        windowText = text;
        if (hwnd)
        {
#if UNICODE
          auto wstr = win::utf8_to_wstring(text);
          return (0 == ::SetWindowText(hwnd, wstr.c_str()));
#else
          return (0 == ::setWindowText(hwnd, text.c_str()));
#endif
        }
        return true;
      }
      return false;
    }

    void BaseWindow::setFont(int size, const TCHAR* font)
    {
      if (hFont)
        ::DeleteObject(hFont);

      hFont = ::CreateFont(-size, 0, 0, 0, FW_DONTCARE, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, font);

      ::SendMessage(hwnd,             // Handle of edit control
        WM_SETFONT,         // Message to change the font
        (WPARAM)hFont,     // handle of the font
        MAKELPARAM(TRUE, 0) // Redraw text
      );
    }

    // Subclasses a WindowProc by storing the actual WNDPROC pointer
    // to patchedWndProc and replacing it with our (static) basicWndProc,
    // which will call handleWindowMessage in our subclasses
    void BaseWindow::SubclassWindowProc(HWND hwnd)
    {
      this->hwnd = hwnd;
      this->patchedWndProc = (WNDPROC)GetWindowLongPtr(hwnd, GWLP_WNDPROC);
      SetWindowLongPtr(hwnd
        , GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(this));
      SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)BaseWindow::basicWndProc);
    }

    void BaseWindow::UpdateDpiDependentFontsAndResources()
    {
      setDefaultFont();
      // TODO: update all other resources that might be registered in basewindow
      // this could be source Bitmaps and/or display resources
    }

    void BaseWindow::resizeToDPI()
    {
      UINT dpi = (UINT)currentDPI;

      UINT uX = MulDiv(rect.x, dpi, 96);
      UINT uY = MulDiv(rect.y, dpi, 96);
      UINT uWidth = MulDiv(rect.w, dpi, 96);
      UINT uHeight = MulDiv(rect.h, dpi, 96);
      SetWindowPos(hwnd, nullptr, uX, uY, uWidth, uHeight, SWP_NOZORDER | SWP_NOACTIVATE);

    }

    LRESULT BaseWindow::handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {

      switch (message)
      {

      case WM_NCDESTROY:
        if (this->patchedWndProc)
        {
          // unpatch the WNDPROC, which is not in the documentation, but
          // Raymond Chen stated this in one of his blog articles
          SetWindowLongPtr(this->hwnd, GWLP_WNDPROC, (LONG_PTR) this->patchedWndProc);
          auto temp = this->patchedWndProc;
          this->patchedWndProc = nullptr; // remove the connection completely
          // call the original message handler
          return temp(this->hwnd, message, wParam, lParam);
        }
        break;
      case WM_DPICHANGED_AFTERPARENT:
      {
        //OutputDebugString(utf8_to_wstring(fmt::format("DPI changed HWND {:x}\n", (uint64_t) hwnd)).c_str());
        //currentDPI = GetWindowDPI(hwnd);
        //resizeToDPI();
      }
      break;
      case UWM_DPICHANGED:
      case WM_DPICHANGED:
      {
        /*
          RECT rect;
          GetWindowRect(hwnd, &rect);
          SetWindowPos(hwnd, hwnd, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
          */
        DWORD newDPI = HIWORD(wParam);
        if (currentDPI != newDPI)
        {
          currentDPI = newDPI;

          UpdateDpiDependentFontsAndResources();

          // WM_DPICHANGED comes with lParam pointing to a suggested new rect for this control
          if (lParam != 0)
          {
            LPRECT r = (LPRECT)lParam;
            SetWindowPos(hwnd, 0, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
          }
          else
          {
            resizeToDPI();
          }

          win::enumWin32ChildWindows(hwnd,
            [&](HWND child)
            {
              auto* self = reinterpret_cast<BaseWindow*>(GetWindowLongPtr(child, GWLP_USERDATA));
              if (self)
              {
                self->basicWndProc(child, UWM_DPICHANGED, wParam, 0);
              }
            }
          );
        }
      }
      break;
      // default behaviour, we might obtain our window and ask it for a background color
      case WM_CTLCOLORSTATIC:
      {
        auto hdc = (HDC)wParam;
        auto wnd = (HWND)lParam;
        SetBkMode(hdc, TRANSPARENT);
        return 0; //  (LRESULT)GetStockBrush(COLOR_WINDOWTEXT);
      }
      break;

      case WM_COMMAND:
        // OutputDebugString(utf8_to_wstring(fmt::format("BASE HWND {3:p}:MSG {0}, {1:x},{2:x}\n","WM_COMMAND", wParam, lParam, (void*)hwnd)).c_str());
        if (lParam)
        {
          auto notificationcode = HIWORD(wParam);
          if (notificationcode == EN_CHANGE)
          {
            static bool ignoreNext = false;
            if (!ignoreNext)
            {
              auto win = (BaseWindow*)(GetWindowLongPtr((HWND)lParam, GWLP_USERDATA));
              if (win)
              {
                std::string text = win->getText(0);
                auto newCaretIndex = Edit_GetCaretIndex(win->getHWND());

                int32_t pos = newCaretIndex;
                if (win->viewHandle.validateContent(text, pos))
                {
                  ignoreNext = true;
                  win->setText(text, 0);
                  // if we name the variable different, the macro does not work.. *sigh*
                  newCaretIndex = pos;
                  Edit_SetCaretIndex(win->getHWND(), newCaretIndex);
                }
              }
            }
            else
            {
              // this means, that the setText came from us and we shall not
              // process this again
              ignoreNext = false;
            }
          }
        }
        break;
      default:
        break;
      }

      if (this->viewHandle.wantsEvent(event::type::click))
      {
        switch (message)
        {
        case WM_LBUTTONDOWN:
          {
          event::Clicked e{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam),0 };
          this->viewHandle.sendEvent(e);
          }
          break;
        }
      }
      // if the derived class subclassed a CommonControl window, the messages are passed to the
      // original WndProc of the control (which might finally end in DefWindowProc() as well).
      if (patchedWndProc)
      {
        return CallWindowProc(patchedWndProc, hwnd, message, wParam, lParam);
      }
      return DefWindowProc(hwnd, message, wParam, lParam);
    }

    void BaseWindow::setParent(RefPtr<IPlatformView> parent)
    {
      this->parentView = parent;
    }

    std::string BaseWindow::getText(int32_t index)
    {
      if (index == 0)
      {
#if UNICODE
        std::wstring r;
        auto len = ::Edit_GetTextLength(hwnd);
        r.resize(len);
        ::Edit_GetText(hwnd, &r[0], len + 1);
        return wstring_to_utf8(r);
#else
        std::string r;
        auto len = ::Edit_GetTextLength(hwnd);
        r.resize(len + 1);
        ::Edit_GetText(hwnd, &r[0], len);
        return r;
#endif
      }
      return std::string();
    }

    void BaseWindow::setDefaultFont()
    {
      if (hFont)
      {
        ::DeleteFont(hFont);
        hFont = 0;
      }
      auto fontsize = -MulDiv(11, currentDPI, 96);
      hFont = CreateFont(fontsize, 0, 0, 0, FW_DONTCARE, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, _T("sans serif"));
      ::SendMessage(hwnd,             // Handle of edit control
        WM_SETFONT,         // Message to change the font
        (WPARAM)hFont,     // handle of the font
        MAKELPARAM(TRUE, 0) // Redraw text
      );
    }

    HWND BaseWindow::getParentHWND() const
    {
      if (parentView)
      {
        return NativeHandle(parentView->getNativeHandle());
      }
      return NULL;
    }

    LRESULT CALLBACK BaseWindow::basicWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
      // https://devblogs.microsoft.com/oldnewthing/20191014-00/?p=102992

      // checking if a class is already there
      BaseWindow* self = (BaseWindow*)(GetWindowLongPtr(hwnd, GWLP_USERDATA));
      if (message == WM_NCCREATE) {
        LPCREATESTRUCT lpcs = reinterpret_cast<LPCREATESTRUCT>(lParam);
        self = static_cast<BaseWindow*>(lpcs->lpCreateParams);
        self->hwnd = hwnd;
        SetWindowLongPtr(hwnd
          , GWLP_USERDATA,
          reinterpret_cast<LONG_PTR>(self));
      }
      //if (message == WM_NCDESTROY)
      //{
      //  BaseWindow* self = (BaseWindow*)(GetWindowLongPtr(hwnd, GWLP_USERDATA));
      //  if (self->patchedWndProc)
      //  {
      //    SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)self->patchedWndProc);
      //  }
      //}
      if (self)
      {
        // prehandling important messages
        switch (message)
        {
        case WM_CREATE:
          self->currentDPI = GetWindowDPI(hwnd);
          break;
        case WM_DPICHANGED:
          OutputDebugString(_T("DPI_CHANGED\n"));
          break;

        default:
          break;
        }
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
  }


}