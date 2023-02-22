#include "windowsrenderer.h"


#include <stdlib.h>
#include <malloc.h>
#include <memory.h>
#include <wchar.h>
#include <math.h>

#include <winrt/Windows.UI.ViewManagement.h>

using namespace winrt;

namespace viewmgmt = winrt::Windows::UI::ViewManagement;


/*

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

#define M_PI   3.14159265358979323846264338327950288f

#ifndef HINST_THISCOMPONENT
EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#define HINST_THISCOMPONENT ((HINSTANCE)&__ImageBase)
#endif

namespace neui
{
  namespace gfx
  {

    namespace gdi
    {
      void f()
      {
        viewmgmt::UISettings uisettings;
        const auto color{ uisettings.GetColorValue(viewmgmt::UIColorType::Background) };
      }

      struct RenderState
      {
        COLORREF pen;
        COLORREF brush;
        XFORM xform;
      };

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

        int getDpi() const { return _dpi; }
        bool isScaled() const { return _dpi != 96; }
        void doScale(const bool scale)
        {
          _doscale = scale;
          // todo: any coords stored in members might need to be converted
        }

        IRenderer& begin() override
        {
          _hdc = BeginPaint(_hwnd, &_ps);
          auto enh = SetGraphicsMode(_hdc, GM_ADVANCED);
          auto xz = ((float)_dpi) / 96.f;
          auto yz = ((float)_dpi) / 96.f;
          XFORM p = { xz,0.,0.,yz,0.,0. };
          SetWorldTransform(_hdc, &p);
          
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
          RenderState s;
          s.pen = GetDCPenColor(_hdc);
          s.brush = GetDCBrushColor(_hdc);
          GetWorldTransform(_hdc, &s.xform);

          _states.push_back(s);
          return *this;
        }

        IRenderer& pop() override
        {
          auto s = _states.back();
          _states.pop_back();
          SetDCPenColor(_hdc, s.pen);
          SetDCBrushColor(_hdc, s.brush);
          SetWorldTransform(_hdc, &s.xform);
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
          ::MoveToEx(_hdc, from.x, from.y, NULL);
          ::LineTo(_hdc, to.x, to.y);
          return *this;
        }

        IRenderer& rect(const Rect rect) override
        {
          SelectObject(_hdc, GetStockObject(NULL_BRUSH));
          ::Rectangle(_hdc, rect.x, rect.y, rect.x + rect.w, rect.y + rect.h);
          SelectObject(_hdc, GetStockObject(DC_BRUSH));
          return *this;
        }
        IRenderer& rect(const Rect rect, uint distance) override
        {
          SelectObject(_hdc, GetStockObject(NULL_BRUSH));
          ::RoundRect(_hdc, rect.x, rect.y, rect.x + rect.w, rect.y + rect.h, distance, distance);
          SelectObject(_hdc, GetStockObject(DC_BRUSH));
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
        IRenderer& translate(const Size offset) override
        {
          XFORM p = { 1.,0.,0.,1.,(float)(offset.w),(float)(offset.h) };
          ModifyWorldTransform(_hdc, &p, MWT_RIGHTMULTIPLY);
          return *this;
        }
        IRenderer& rotate(const Point center, float normalized_angle) override
        {
          auto x0 = (float)center.x;
          auto y0 = (float)center.y;

          auto a = normalized_angle * (M_PI / 180.f);
          XFORM r2;
          auto c = cos(a);
          auto s = sin(a);
          r2.eM11 = c;
          r2.eM12 = s;
          r2.eM21 = -s;
          r2.eM22 = c;
          r2.eDx = 0.f; //  x0 - c * x0 + s * y0;
          r2.eDy = 0.f; // y0 - c * y0 - s * x0;
          ModifyWorldTransform(_hdc, &r2, MWT_RIGHTMULTIPLY);
          return *this;
        }
        IRenderer& text(const std::string_view text, const Rect rect, uint ninealign) override
        {
          return *this;
        }

      private:
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
        std::vector<RenderState> _states;
        // Inherited via IRenderer
       
      };

      // factory function for context
      std::shared_ptr<IRenderer> make(HWND hwnd, const Rect rect)
      {
        return std::make_shared<Renderer>(hwnd, Rect());
      }
    }
  }
}