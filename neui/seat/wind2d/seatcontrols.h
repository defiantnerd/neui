#pragma once

#define NOMINMAX 1
#include <Windows.h>
#include <CommCtrl.h>
#include "common/common.h"
#include "../base/baseseat.h"
#include "base.h"

extern HINSTANCE gInstance;

namespace neui
{
  namespace wind2d
  {    
    inline HWND NativeHandle(void* nativeHandle)
    {
      return (HWND)nativeHandle;
    }

    inline HWND NativeHandle(IPlatformView& widget)
    {
      return (HWND)(widget.getNativeHandle());
    }

    HFONT DefaultFont();

    class VirtualWidget : public IPlatformView
    {
    public:
      void setViewHandle(const ViewHandle& handle) override;
      void setRect(const Rect& rect) override;
      void setBoxModel(const BoxModel& bm) override;
    protected:
      ViewHandle viewHandle;
      BoxModel boxmodel;
      Rect rect;
    };

    class Win32Window : public VirtualWidget
    {
    public:
      void* getNativeHandle() const override { return (void*)(getHWND()); }
      virtual HWND getHWND() const = 0;
    };
    
    class ClassInstance
    {
    public:
      virtual ATOM getAtom() const = 0;
      virtual ~ClassInstance() {}
    };


    class BaseWindow : public Win32Window
    {
    public:
      ~BaseWindow() override;
      void setAlpha(int percent) override;
      float getDpi() override;
      void destroy() override;
      void show(int show = SW_SHOWDEFAULT) override;
      void hide() override { show(SW_HIDE); }
      void enable() override;
      void disable() override;
      bool setText(const std::string_view text, int32_t index) override;
      bool setInteger(const int32_t value, int32_t index) override;
      void focus();
      HWND getHWND() const final { return hwnd; }
      static LRESULT CALLBACK basicWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
      void setParent(RefPtr<IPlatformView> parent) override;
      std::string getText(int32_t index);
      void animate();
    protected:
      BaseWindow() = default;
      void create() override;
      void setDefaultFont();
      HWND getParentHWND() const;
      void setFont(int size, const TCHAR* font);
      void SubclassWindowProc(HWND hwnd);
      virtual LRESULT handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam);

      virtual void UpdateDpiDependentFontsAndResources();
      void resizeToDPI();

      RECT upscaleRECT(const Rect& r);
      Rect upscaleRect(const Rect& r);


      HWND hwnd = 0;
      WNDPROC patchedWndProc = 0;
      HFONT hFont = 0;
      DWORD currentDPI = 96;
      bool retrigger = false;
      // DPI on this window?
      void registerClass(WNDCLASSEX& wc); // registers a class. if already registered, it keeps track of all instances
      ATOM getClassAtom() const { return classInstance->getAtom(); }
      RefPtr<IPlatformView> parentView = nullptr;
      std::string windowText;
      std::shared_ptr<IRenderer> renderer = nullptr;
    private:
      
      std::shared_ptr<ClassInstance> classInstance;

    };

  }

}
