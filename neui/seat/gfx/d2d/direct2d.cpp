#include "direct2d.h"

#include <stdlib.h>
#include <malloc.h>
#include <memory.h>
#include <wchar.h>
#include <math.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>


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
          SelectObject(_hdc, GetStockObject(WHITE_PEN));
          return *this;
        }
        IRenderer& end() override
        {
          EndPaint(_hwnd, &_ps);
          return *this;
        }
        // 
        IRenderer& line(const Point from, const Point to) override
        {
          ::MoveToEx(_hdc, scaled(from.x), scaled(from.y), NULL);
          ::LineTo(_hdc, scaled(to.x), scaled(to.y));
          return *this;
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
        virtual IRenderer& rect(const Rect rect) override
        {
          return *this;
        }
        virtual IRenderer& rect(const Rect rect, uint distance) override
        {
          return *this;
        }
        virtual IRenderer& circle(const Point center, const uint r) override
        {
          return *this;
        }
        virtual IRenderer& ellipse(const Point center, const uint rx, const uint ry) override
        {
          return *this;
        }
        virtual IRenderer& pushclip(const Rect rect) override
        {
          return *this;
        }
        virtual IRenderer& popclip() override
        {
          return *this;
        }
        virtual IRenderer& transpose(const Size offset) override
        {
          return *this;
        }
        virtual IRenderer& rotate(const Point center, float normalized_angle) override
        {
          return *this;
        }
        virtual IRenderer& text(const std::string_view text, const Rect rect, uint ninealign) override
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