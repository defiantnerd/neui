#include "windowsrenderer.h"


#include <stdlib.h>
#include <malloc.h>
#include <memory.h>
#include <wchar.h>
#include <math.h>

#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>

#include <Uxtheme.h>

#include <winrt/Windows.UI.ViewManagement.h>

#define M_PI   3.14159265358979323846264338327950288f

using namespace winrt;

namespace viewmgmt = winrt::Windows::UI::ViewManagement;

#pragma comment(lib, "d2d1.lib")

/*
      this is the direct2d implementation for the drawing context
      In general, the context for a HWND should be preserved since allocation of
      resources can be skipped under most circumstances

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

      the context state stores the current transformation matrix, the current pen/brush colors
      and the ContextState from the Direct2D engine. Direct2d can not add transforms so a copy of
      the transform is being kept and modified to be applied.

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

D2D1_SIZE_F asSize2F(const neui::Point& p)
{
  return { (FLOAT)p.x,(FLOAT)p.y };
}

D2D1_SIZE_F asSize2F(const neui::Size& p)
{
  return { (FLOAT)p.w,(FLOAT)p.h };
}


D2D1_POINT_2F asPoint2F(const neui::Point& p)
{
  return D2D1_POINT_2F{ (FLOAT) p.x, (FLOAT) p.y};
}

D2D1_RECT_F asRect2F(const neui::Rect& r)
{
  return {(FLOAT)r.x,(FLOAT)r.y,(FLOAT)r.x+r.w,(FLOAT)r.y+r.h };
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

      struct ContextState
      {
        ID2D1DrawingStateBlock* _drawingState = nullptr;
        D2D1::Matrix3x2F _transform = D2D1::Matrix3x2F();
        uint32_t _pencolor = 0;
        uint32_t _brushcolor = 0;
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
          CreateDeviceIndependentResources();
        }

        ~Renderer()
        {
          SafeRelease(&_direct2dFactory);
        }

        void resize(const Size newsize) override
        {
          if (_renderTarget)
          {
            _renderTarget->Resize(D2D1_SIZE_U{(UINT32)newsize.w,(UINT32)newsize.h});
              auto dpi = ::GetDpiForWindow(_hwnd);
              _renderTarget->SetDpi(dpi, dpi);
          }
        }

        IRenderer& begin() override
        {

          BeginPaint(_hwnd, &ps);

          HRESULT hr = S_OK;
          CreateDeviceResources();
          _renderTarget->BeginDraw();
          _currenttransform = D2D1::Matrix3x2F::Identity(); // Translation({ +0.5,+0.5 });
          _renderTarget->SetTransform(_currenttransform);
          // _renderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
          _renderTarget->Clear(D2D1::ColorF(D2D1::ColorF::White));
          return *this;
        }
        IRenderer& end() override
        {
          auto hr = _renderTarget->EndDraw();
          if (hr == D2DERR_RECREATE_TARGET)
          {
            hr = S_OK;
            DiscardDeviceResources();
          }
          EndPaint(_hwnd, &ps);
          return *this;
        }
        
        IRenderer& push() override
        {
          ID2D1DrawingStateBlock* n = nullptr;
          auto hr = _direct2dFactory->CreateDrawingStateBlock(&n);
          if (hr == S_OK && n)
          {
            _renderTarget->SaveDrawingState(n);
          }
          _stack.push_back(ContextState{ n, _currenttransform, _pencolor, _brushcolor });
          return *this;
          
        }

        IRenderer& pop() override
        {
          auto n = _stack.back();
          if (n._drawingState)
          {
            _renderTarget->RestoreDrawingState(n._drawingState);
            _currenttransform = n._transform;
            _renderTarget->SetTransform(_currenttransform);
            pen(n._pencolor);
            brush(n._brushcolor);
          }
          _stack.pop_back();
          SafeRelease(&n._drawingState);
          return *this;
        }

        IRenderer& pen(uint32_t color) override 
        {
          D2D_COLOR_F c;
          c.a = 1.f;
          c.r = (color & 0xff) / 255.;
          c.g = ((color>>8) & 0xff) / 255.;
          c.b = ((color >> 16) & 0xff) / 255.;
          _pen->SetColor(c);
          _pencolor = color;
          return *this;
        }

        IRenderer& brush(uint32_t color) override
        {
          D2D_COLOR_F c;
          c.a = 1.f;
          c.r = (color & 0xff) / 255.;
          c.g = ((color >> 8) & 0xff) / 255.;
          c.b = ((color >> 16) & 0xff) / 255.;
          _brush->SetColor(c);
          _brushcolor = color;
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
        //inline int scaled(const int n)
        //{
        //  if (_doscale)
        //    return MulDiv(n, _dpi, 96);
        //  return n;
        //}
#if 1
        // Initialize device-independent resources.
        HRESULT CreateDeviceIndependentResources();

        // Initialize device-dependent resources.
        HRESULT CreateDeviceResources();

        // Release device-dependent resource.
        void DiscardDeviceResources();

        ID2D1Factory* _direct2dFactory = nullptr;
        ID2D1HwndRenderTarget* _renderTarget = nullptr;
        ID2D1DeviceContext* _deviceContext = nullptr;
#endif

        HWND _hwnd;
        HDC _hdc;
        Rect _updaterect;
        int _dpi = 96;
        bool _doscale = true;  
        PAINTSTRUCT ps = {};

        D2D1::Matrix3x2F _currenttransform;

        ID2D1SolidColorBrush* _pen = nullptr;
        ID2D1SolidColorBrush* _brush = nullptr;
        uint32_t _pencolor = 0;
        uint32_t _brushcolor = 0;

        std::vector<ContextState> _stack;

        IRenderer& line(const Point from, const Point to) override
        {
          D2D1_POINT_2F p1{ (float)from.x, (float)from.y };
          D2D1_POINT_2F p2{ (float)to.x, (float)to.y };
          _renderTarget->DrawLine(p1, p2, _pen);
          return *this;
        }

        // Inherited via IRenderer
        IRenderer& rect(const Rect rect) override
        {
          _renderTarget->DrawRectangle(asRect2F(rect), _pen);
          return *this;
        }
        IRenderer& rect(const Rect rect, uint distance) override
        {
          D2D1_ROUNDED_RECT r{ asRect2F(rect), (FLOAT)distance, (FLOAT) distance};
          _renderTarget->DrawRoundedRectangle(r, _pen);
          return *this;
        }
        IRenderer& circle(const Point center, const uint r) override
        {
          D2D1_ELLIPSE e{ asPoint2F(center), (FLOAT)r, (FLOAT)r };
          _renderTarget->DrawEllipse(e, _pen);
          return *this;
        }
        IRenderer& ellipse(const Point center, const uint rx, const uint ry) override
        {
          D2D1_ELLIPSE e{ asPoint2F(center), (FLOAT)rx, (FLOAT)ry };
          _renderTarget->DrawEllipse(e, _pen);
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
          _currenttransform = _currenttransform * D2D1::Matrix3x2F::Translation(asSize2F(offset));
          _renderTarget->SetTransform(_currenttransform);
          return *this;
        }
        IRenderer& rotate(const Point center, float normalized_angle) override
        {
          _currenttransform = 
            _currenttransform * D2D1::Matrix3x2F::Rotation(normalized_angle , asPoint2F(center));
          _renderTarget->SetTransform(_currenttransform);
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
      HRESULT Renderer::CreateDeviceIndependentResources()
      {
        return D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &_direct2dFactory);
      }

      HRESULT Renderer::CreateDeviceResources()
      {
        if (_renderTarget)
          return S_OK;

        HRESULT hr;
        RECT rc;

        GetClientRect(_hwnd, &rc);

        D2D1_SIZE_U size = D2D1::SizeU(
          rc.right - rc.left,
          rc.bottom - rc.top);

        // Create a Direct2D render target.
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
        
        props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;

        hr = _direct2dFactory->CreateHwndRenderTarget(
          props,
          D2D1::HwndRenderTargetProperties(_hwnd, size, D2D1_PRESENT_OPTIONS_IMMEDIATELY),
          &_renderTarget);

        if (SUCCEEDED(hr))
        {
          _renderTarget->QueryInterface(
            __uuidof(ID2D1DeviceContext),
            reinterpret_cast<void**>(&_deviceContext)
          );
        }

        if (SUCCEEDED(hr))
        {
          auto dpi = ::GetDpiForWindow(_hwnd);
          _renderTarget->SetDpi(dpi, dpi);
        }

        if (SUCCEEDED(hr))
        {
          // Create a gray brush.
          hr = _renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::White),
            &_brush
          );
        }

        if (SUCCEEDED(hr))
        {
          // Create a black brush.
          hr = _renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::Black),
            &_pen
          );
        }

        return hr;
      }

      void Renderer::DiscardDeviceResources()
      {
        SafeRelease(&_pen);
        SafeRelease(&_brush);
        SafeRelease(&_deviceContext);
        SafeRelease(&_renderTarget);
      }
}
  }
}