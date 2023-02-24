#include "windowsrenderer.h"


#include <stdlib.h>
#include <malloc.h>
#include <memory.h>
#include <wchar.h>
#include <math.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>

#include <Uxtheme.h>

#include <winrt/Windows.UI.ViewManagement.h>

using namespace winrt;

namespace viewmgmt = winrt::Windows::UI::ViewManagement;


/*
      yeah, right now this is GDI instead of direct2d, but since the interface is
      not completely designed and still evolving, GDI is being used to draw for the
      time being for simpler changes.

      Current tasks:
      - enable scaled and non scaled (native) mode
      - draw basic shapes
      - develop the text interface
      - allow access to text metrics (font sizes in contrast to resolution)

      regarding scaling:

      the idea is that the render interface works in scaled mode by default:

      coordinates are 1x despite any DPI setting of the screen and they are all
      converted internally. To access the real resolution the functions getDPI() and doScale(bool)
      can be used to access all coordinates and refine drawing on high res displays.

*/

template<class Interface>
inline void SafeRelease(
  Interface** ppInterfaceToRelease)
{
  if (*ppInterfaceToRelease != NULL)
  {
    (*ppInterfaceToRelease)->Release();
    (*ppInterfaceToRelease) = NULL;
  }
}

#ifndef Assert
#if defined( DEBUG ) || defined( _DEBUG )
#define Assert(b) do {if (!(b)) {OutputDebugStringA("Assert: " #b "\n");}} while(0)
#else
#define Assert(b)
#endif //DEBUG || _DEBUG
#endif

#ifndef HINST_THISCOMPONENT
EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#define HINST_THISCOMPONENT ((HINSTANCE)&__ImageBase)
#endif

namespace neui
{
  namespace gfx
  {

    namespace d2d
    {
      void f()
      {
        viewmgmt::UISettings uisettings;
        const auto color{ uisettings.GetColorValue(viewmgmt::UIColorType::Background) };
      }

      class Renderer : public IRenderer
      {
      public:
        Renderer(HWND handle, Rect updaterect)
          : _hwnd(handle)
          , _hdc(0)
          , _updaterect(updaterect) 
          , _dpi(GetDpiForWindow(handle))
        {
        }

        IRenderer& begin() override
        {
          _hdc = BeginPaint(_hwnd, &_ps);
          SelectObject(_hdc, GetStockObject(DC_BRUSH));
          SelectObject(_hdc, GetStockObject(DC_PEN));
          SetDCPenColor(_hdc, 0x00000000);
          SetDCBrushColor(_hdc, 0x00ffffff);
          return *this;
        }
        IRenderer& end() override
        {
          EndPaint(_hwnd, &_ps);
          return *this;
        }
        
        IRenderer& push() override
        {
          return *this;
        }

        IRenderer& pop() override
        {
          return *this;
        }

        IRenderer& pen(uint32_t color) override 
        {
          SetDCPenColor(_hdc, color);
          return *this;
        }

        IRenderer& brush(uint32_t color) override
        {
          SetDCBrushColor(_hdc, color);
          return *this;
        }


        IRenderer& line(const Point from, const Point to) override
        {
          ::MoveToEx(_hdc, scaled(from.x), scaled(from.y), NULL);
          ::LineTo(_hdc, scaled(to.x), scaled(to.y));
          return *this;
        }

        int getDpi() const { return _dpi; }
        bool isScaled() const { return _dpi != 96; }
        void doScale(const bool scale)
        {
          _doscale = scale;
          // todo: any coords stored in members might need to be converted
        }
      private:
        inline int scaled(const int n)
        {
          if (_doscale)
            return MulDiv(n, _dpi, 96);
          return n;
        }
#if 0
        // Initialize device-independent resources.
        HRESULT CreateDeviceIndependentResources();

        // Initialize device-dependent resources.
        HRESULT CreateDeviceResources();

        // Release device-dependent resource.
        void DiscardDeviceResources();

        HWND m_hwnd;
        ID2D1Factory* m_pDirect2dFactory;
        ID2D1HwndRenderTarget* m_pRenderTarget;
#endif

        HWND _hwnd;
        HDC _hdc;
        Rect _updaterect;
        int _dpi = 96;
        bool _doscale = true;
        PAINTSTRUCT _ps = PAINTSTRUCT();
        // Inherited via IRenderer
        IRenderer& rect(const Rect rect) override
        {
          return *this;
        }
        IRenderer& rect(const Rect rect, uint distance) override
        {
          return *this;
        }
        IRenderer& circle(const Point center, const uint r) override
        {
          return *this;
        }
        IRenderer& ellipse(const Point center, const uint rx, const uint ry) override
        {
          return *this;
        }
        IRenderer& pushclip(const Rect rect) override
        {
          return *this;
        }
        IRenderer& popclip() override
        {
          return *this;
        }
        IRenderer& translate(const Point offset) override
        {

          return *this;
        }
        IRenderer& rotate(const Point center, float normalized_angle) override
        {
          return *this;
        }
        IRenderer& text(const std::string_view text, const Rect rect, uint ninealign) override
        {
          return *this;
        }
      };

      // factory function for context
      std::shared_ptr<IRenderer> make(HWND hwnd, const Rect rect)
      {
        return std::make_shared<Renderer>(hwnd, Rect());
      }
    }
  }
}