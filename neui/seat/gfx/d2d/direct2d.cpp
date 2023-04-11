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

      struct Factories_t
      {
        ID2D1Factory* d2dfactory = nullptr;
        ~Factories_t() {
          if (d2dfactory)
            SafeRelease(&d2dfactory);
        }
      } factories;

      class AssetImpl
      {
      private:
        // -------------------------------------------------
        struct CommandRefs
        {
          ID2D1DeviceContext* _dc = nullptr;
          const Asset& _data;
        };
        class Command
        {
        public:
          Command(CommandRefs& refs) : _refs(refs) {}
          virtual ~Command() {}
          virtual void draw() = 0;
          virtual void discard() = 0;
        protected:
          inline D2D1_COLOR_F toD2D1(const uint32_t color_argb)
          {
            D2D1_COLOR_F result;
            result.a = float((color_argb >> 24) & 0xFF) / 255.f;
            result.r = float((color_argb >> 16) & 0xFF) / 255.f;
            result.g = float((color_argb >> 8) & 0xFF) / 255.f;
            result.b = float(color_argb & 0xFF) / 255.f;
            return result;
          }
          inline D2D1_POINT_2F toD2D1(const tvg::Point& p)
          {
            return { p.x,p.y };
          }
          inline D2D1_RECT_F toD2D1(const tvg::Rectangle& r)
          {
            return { r.x, r.y, r.x+r.w, r.y+r.h };
          }
          ID2D1Brush* createBrushFromStyle(const tvg::Style& style);
          ID2D1PathGeometry* createPathFromTVGPolygon(const std::vector<tvg::Point>& points, bool closed);
          ID2D1PathGeometry* createPathFromTVGPath(const tvg::Path& path);
          ID2D1PathGeometry* createPathFromTVGRectangles(const std::vector<tvg::Rectangle>& rects);
          CommandRefs& _refs;
        };

        class FillPolygon : public Command
        {
        public:
          FillPolygon(CommandRefs& refs, const tvg::FillPolygon& fill_polygon);
          void draw() override;
          void discard() override;
          ~FillPolygon() override;
        private:
          void allocate();
          const tvg::FillPolygon& _data;
          ID2D1Brush* _brush = nullptr; 
          ID2D1PathGeometry* _path = nullptr;
        };

        class OutlineFillPolygon : public Command
        {
        public:
          OutlineFillPolygon(CommandRefs& refs, const tvg::OutlineFillPolygon& fill_polygon);
          void draw() override;
          void discard() override;
          ~OutlineFillPolygon() override;
        private:
          void allocate();
          const tvg::OutlineFillPolygon& _data;
          ID2D1Brush* _brush1 = nullptr;
          ID2D1Brush* _brush2 = nullptr;
          ID2D1PathGeometry* _path = nullptr;
        };

        class FillRectangles : public Command
        {
        public:
          FillRectangles(CommandRefs& refs, const tvg::FillRectangles& fill_rectangles)
            : Command(refs)
            , _data(fill_rectangles)
          {}
          void draw() override {
            if (!_brush && !_path)
              allocate();
            if (_brush && _path)
            {
              _refs._dc->FillGeometry(_path, _brush, nullptr);
            }
          }
          void discard() override {
            SafeRelease(&_brush);
          }
          ~FillRectangles() override {
            SafeRelease(&_path);
          }
        private:
          void allocate() {
            _brush = createBrushFromStyle(_data.prim_style);
            _path = createPathFromTVGRectangles(_data.rectangles);
          }
          const tvg::FillRectangles& _data;
          ID2D1Brush* _brush = nullptr;
          ID2D1PathGeometry* _path = nullptr;
        };

        class OutlineFillRectangles : public Command
        {
        public:
          OutlineFillRectangles(CommandRefs& refs, const tvg::OutlineFillRectangles& fill_rectangles)
            : Command(refs)
            , _data(fill_rectangles)
          {}
          void draw() override {
            if (!_brush1 && !_path)
              allocate();
            if (_brush1 && _brush2 && _path)
            {
              _refs._dc->FillGeometry(_path, _brush1, nullptr);
              _refs._dc->DrawGeometry(_path, _brush2, _data.linewdith);
            }
          }
          void discard() override {
            SafeRelease(&_brush1);
            SafeRelease(&_brush2);
          }
          ~OutlineFillRectangles() override {
            SafeRelease(&_path);
          }
        private:
          void allocate() {
            _brush1 = createBrushFromStyle(_data.prim_style);
            _brush2 = createBrushFromStyle(_data.sec_style);
            _path = createPathFromTVGRectangles(_data.rectangles);
          }
          const tvg::OutlineFillRectangles& _data;
          ID2D1Brush* _brush1 = nullptr;
          ID2D1Brush* _brush2 = nullptr;
          ID2D1PathGeometry* _path = nullptr;
        };

        class FillPath : public Command
        {
        public:
          FillPath(CommandRefs& refs, const tvg::FillPath& fill_path);
          void draw() override;
          void discard() override;
          ~FillPath() override;
        private:
          void allocate();
          const tvg::FillPath& _data;
          ID2D1Brush* _brush = nullptr;
          ID2D1PathGeometry* _path = nullptr;
        };

        class OutlineFillPath : public Command
        {
        public:
          OutlineFillPath(CommandRefs& refs, const tvg::OutlineFillPath& fill_path);
          void draw() override;
          void discard() override;
          ~OutlineFillPath() override;
        private:
          void allocate();
          const tvg::OutlineFillPath& _data;
          ID2D1Brush* _brush1 = nullptr;
          ID2D1Brush* _brush2 = nullptr;
          ID2D1PathGeometry* _path = nullptr;
        };

        class DrawLines : public Command
        {
        public:
          DrawLines(CommandRefs& refs, const tvg::DrawLines& draw_lines)
            : Command(refs)
            , _data(draw_lines) {}
          void draw() override
          {
            if (!_brush) allocate();
            if (_brush)
            {
              for (auto& l : _data.lines)
              {
                _refs._dc->DrawLine(toD2D1(l.start), toD2D1(l.end), _brush, _data.linewidth);
              }
            }
          }
          void discard() override { SafeRelease(&_brush); }
          ~DrawLines() {}
        private:
          void allocate()
          {
            _brush = createBrushFromStyle(_data.prim_style);
          }
          const tvg::DrawLines& _data;
          ID2D1Brush* _brush = nullptr;
        };

        class DrawPolygon : public Command
        {
        public:
          DrawPolygon(CommandRefs& refs, const tvg::DrawLineLoop& draw_lines)
            : Command(refs)
            , _data(draw_lines) {}
          void draw() override
          {
            if (!_brush) allocate();
            if (_brush)
            {
              _refs._dc->DrawGeometry(_path, _brush, _data.linewidth);
            }
          }
          void discard() override { SafeRelease(&_brush); }
          ~DrawPolygon() { SafeRelease(&_path); }
        private:
          void allocate()
          {
            _brush = createBrushFromStyle(_data.prim_style);
            _path = createPathFromTVGPolygon(_data.points, true);
          }
          const tvg::DrawLineLoop& _data;
          ID2D1Brush* _brush = nullptr;
          ID2D1PathGeometry* _path = nullptr;

        };

        class DrawLineStrip : public Command
        {
        public:
          DrawLineStrip(CommandRefs& refs, const tvg::DrawLineStrip& draw_line_strip)
            : Command(refs)
            , _data(draw_line_strip) {}
          void draw()
          {
            if (!_brush) allocate();
            if (_brush)
            {
              _refs._dc->DrawGeometry(_path, _brush, _data.linewidth);
            }
          }
          void discard() { SafeRelease(&_brush); }
          ~DrawLineStrip() { SafeRelease(&_path); }
        private:
          void allocate()
          {
            _brush = createBrushFromStyle(_data.prim_style);
            _path = createPathFromTVGPolygon(_data.points, false);
          }
          const tvg::DrawLineStrip& _data;
          ID2D1Brush* _brush = nullptr;
          ID2D1PathGeometry* _path = nullptr;

        };

        class DrawPath : public Command
        {
        public:
          DrawPath(CommandRefs& refs, const tvg::DrawLinePath& draw_lines)
            : Command(refs)
            , _data(draw_lines) {}
          void draw() override
          {
            if (!_brush) allocate();
            if (_brush)
            {
              _refs._dc->DrawGeometry(_path, _brush, _data.linewidth);
            }
          }
          void discard() override { SafeRelease(&_brush); }
          ~DrawPath() { SafeRelease(&_path); }
        private:
          void allocate()
          {
            _brush = createBrushFromStyle(_data.prim_style);
            _path = createPathFromTVGPath(_data.path);
          }
          const tvg::DrawLinePath& _data;
          ID2D1Brush* _brush = nullptr;
          ID2D1PathGeometry* _path = nullptr;

        };

        // -------------------------------------------------
      public:
        AssetImpl(Asset& data);
        void draw(ID2D1DeviceContext* d2ddc);

      private:
        void prepareNative(ID2D1DeviceContext* d2ddc);

        Asset& _data;
        CommandRefs _refs;
        std::vector<std::unique_ptr<Command>> _nativeCommands;
      };

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
          SafeRelease(&factories.d2dfactory);
        }

        void resize(const Size newsize) override
        {
          if (_renderTarget)
          {
            _renderTarget->Resize(D2D1_SIZE_U{ (UINT32)newsize.w,(UINT32)newsize.h });
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
          _currenttransform = D2D1::Matrix3x2F::Translation({ .5,.5 });
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
          auto hr = factories.d2dfactory->CreateDrawingStateBlock(&n);
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
          c.g = ((color >> 8) & 0xff) / 255.;
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
        
        // ID2D1Factory* _direct2dFactory = nullptr;
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
          D2D1_ROUNDED_RECT r{ asRect2F(rect), (FLOAT)distance, (FLOAT)distance };
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

        IRenderer& draw(const Point origin, Asset& asset) override
        {
          push();
          translate(origin);

          if (!asset._platformdata)
          {
            // create assets platform data
            asset._platformdata = new AssetImpl(asset);
          }
          auto k = static_cast<AssetImpl*>(asset._platformdata);          
          k->draw(_deviceContext);

          pop();
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
            _currenttransform * D2D1::Matrix3x2F::Rotation(normalized_angle, asPoint2F(center));
          _renderTarget->SetTransform(_currenttransform);
          return *this;
        }
        IRenderer& scale(float factor, const Point center ) override
        {
          _currenttransform =
            _currenttransform * D2D1::Matrix3x2F::Scale({ factor,factor }, asPoint2F(center));
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
        if ( !factories.d2dfactory)
          return D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factories.d2dfactory);
        factories.d2dfactory->AddRef();
        return S_OK;
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

        hr = factories.d2dfactory->CreateHwndRenderTarget(
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

      AssetImpl::AssetImpl(Asset& data)
        : _data(data)
        , _refs{ nullptr, data }
      {
      }

      void AssetImpl::prepareNative(ID2D1DeviceContext* d2ddc)
      {
        if (this->_nativeCommands.empty())
        {
          _refs._dc = d2ddc;
          for (const auto& i : _data.commands)
          {
            switch (i->index)
            {
              case 1: // fill_polygon
                _nativeCommands.push_back(std::make_unique<FillPolygon>(_refs, static_cast<const tvg::FillPolygon&>(*i)));
                break;
              case 2:
                _nativeCommands.push_back(std::make_unique<FillRectangles>(_refs, static_cast<const tvg::FillRectangles&>(*i)));
                break;
              case 3:
                _nativeCommands.push_back(std::make_unique<FillPath>(_refs, static_cast<const tvg::FillPath&>(*i)));
                break;
              case 4:
                _nativeCommands.push_back(std::make_unique<DrawLines>(_refs, static_cast<const tvg::DrawLines&>(*i)));
                break;
              case 5:
                _nativeCommands.push_back(std::make_unique<DrawPolygon>(_refs, static_cast<const tvg::DrawLineLoop&>(*i)));
                break;
              case 6:
                _nativeCommands.push_back(std::make_unique<DrawLineStrip>(_refs, static_cast<const tvg::DrawLineStrip&>(*i)));
                break;
              case 7:
                _nativeCommands.push_back(std::make_unique<DrawPath>(_refs, static_cast<const tvg::DrawLinePath&>(*i)));
                break;
              case 8:
                _nativeCommands.push_back(std::make_unique<OutlineFillPolygon>(_refs, static_cast<const tvg::OutlineFillPolygon&>(*i)));
                break;
              case 9:
                _nativeCommands.push_back(std::make_unique<OutlineFillRectangles>(_refs, static_cast<const tvg::OutlineFillRectangles&>(*i)));
                break;
              case 10:
                _nativeCommands.push_back(std::make_unique<OutlineFillPath>(_refs, static_cast<const tvg::OutlineFillPath&>(*i)));
                break;

            }
          }
        }
      }

      void AssetImpl::draw(ID2D1DeviceContext* d2ddc)
      {
        prepareNative(d2ddc);
        for (auto& c : _nativeCommands)
        {
          c->draw();
        }

      }

      ID2D1Brush* AssetImpl::Command::createBrushFromStyle(const tvg::Style& style)
      {
        ID2D1Brush* result = nullptr;
        if (style.flat)
        {
          ID2D1SolidColorBrush* brush = nullptr;
          auto col = _refs._data.colors[style.type.flat.color_index];
          _refs._dc->CreateSolidColorBrush(toD2D1(col), &brush);
          result = brush;
        }
        else
        {
          if (style.linear)
          {
            ID2D1GradientStopCollection* stops;
            ID2D1LinearGradientBrush* brush;
            auto col1 = _refs._data.colors[style.type.gradient.color_index_0];
            auto col2 = _refs._data.colors[style.type.gradient.color_index_1];

            D2D1_GRADIENT_STOP gradientStops[2];
            gradientStops[0].color = toD2D1(col1);
            gradientStops[0].position = 0.0f;
            gradientStops[1].color = toD2D1(col2);
            gradientStops[1].position = 1.0f;

            _refs._dc->CreateGradientStopCollection(gradientStops, 2, &stops);
            if (stops)
            {
              _refs._dc->CreateLinearGradientBrush(
                { toD2D1(style.type.gradient.point_0) , toD2D1(style.type.gradient.point_1) },
                stops, &brush);
              result = brush;
              // they aren't needed anymore
              SafeRelease(&stops);
            }
          }
          else
          {
            ID2D1GradientStopCollection* stops;
            ID2D1RadialGradientBrush* brush;
            auto col1 = _refs._data.colors[style.type.gradient.color_index_0];
            auto col2 = _refs._data.colors[style.type.gradient.color_index_1];

            D2D1_GRADIENT_STOP gradientStops[2] = {
              {0.f, toD2D1(col1)},
              {1.f, toD2D1(col2)},
            };

            _refs._dc->CreateGradientStopCollection( gradientStops, 2, &stops );
            if (stops)
            {
              auto& grad = style.type.gradient;
              auto& p0 = grad.point_0;
              auto& p1 = grad.point_1;
              auto rx = p1.x - p0.x;
              auto ry = p1.y - p0.y;
              auto r = sqrt((rx * rx) + (ry + ry));

              _refs._dc->CreateRadialGradientBrush(
                { toD2D1(p0) , {0.f,0.f } ,
                r, r },
                stops, &brush);
              result = brush;
              
              SafeRelease(&stops);
            }
          }
        }
        return result;
      }

      ID2D1PathGeometry* AssetImpl::Command::createPathFromTVGRectangles(const std::vector<tvg::Rectangle>& rects) {
        ID2D1PathGeometry* result = nullptr;
        factories.d2dfactory->CreatePathGeometry(&result);
        ID2D1GeometrySink* s = NULL;
        result->Open(&s);
        s->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
        for (const auto& r : rects)
        {
          s->BeginFigure({ r.x,r.y }, D2D1_FIGURE_BEGIN_FILLED);
          auto right = r.x + r.w;
          auto bottom = r.y + r.h;
          D2D1_POINT_2F n[3] = {
            { right, r.y},
            { right, bottom },
            { r.x, bottom }
          };
          s->AddLines(n, 3);
          s->EndFigure(D2D1_FIGURE_END_CLOSED);
        }

        s->Close();
        SafeRelease(&s);
        return result;
      }

      ID2D1PathGeometry* AssetImpl::Command::createPathFromTVGPolygon(const std::vector<tvg::Point>& points, bool closed) {
        ID2D1PathGeometry* result;

        factories.d2dfactory->CreatePathGeometry(&result);
        ID2D1GeometrySink* s = NULL;
        result->Open(&s);
        s->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
        s->SetSegmentFlags(D2D1_PATH_SEGMENT_FORCE_ROUND_LINE_JOIN);
        s->BeginFigure(toD2D1(points[0]), D2D1_FIGURE_BEGIN_FILLED);
        
        if (points.size() <= 64)
        {
          D2D1_POINT_2F n[64];
          auto r = n;
          for (int i = 1; i < points.size(); ++i)
          {
            auto& p = points[i];
            *r++ = { p.x, p.y };
          }
          s->AddLines(n, points.size() - 1);
        }
        else
        {
          auto n = new D2D1_POINT_2F[points.size()];
          auto r = n;
          for (int i = 1; i < points.size(); ++i)
          {
            auto& p = points[i];
            *r++ = { p.x, p.y };
          }
          s->AddLines(n, points.size() - 1);
          delete[] n;
        }
        
        s->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);

        s->Close();

        SafeRelease(&s);

        return result;
      }

      ID2D1PathGeometry* AssetImpl::Command::createPathFromTVGPath(const tvg::Path& path) {
        ID2D1PathGeometry* result = nullptr;
        factories.d2dfactory->CreatePathGeometry(&result);
        ID2D1GeometrySink* s = NULL;
        result->Open(&s);
        s->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
        for (const auto& r : path.segments)
        {
          s->BeginFigure({ r.p0.x,r.p0.y }, D2D1_FIGURE_BEGIN_FILLED);
          tvg::Point last = r.p0; // for horz/vert segments we need to keep and update the last point
          for (const auto& seg : r.node)
          {
            switch (seg.type)
            {
              case 0:
                last = seg.data.line.p2;
                s->AddLine(toD2D1(last));
                break;
              case 1:
                last.x = seg.data.horz.pos;
                s->AddLine(toD2D1(last));
                break;
              case 2:
                last.y = seg.data.vert.pos;
                s->AddLine(toD2D1(last));
                break;
              case 3:
                {
                  last = seg.data.bezier.point_1;
                  D2D1_BEZIER_SEGMENT bs{ toD2D1(seg.data.bezier.control0), toD2D1(seg.data.bezier.control1), toD2D1(last) };
                  s->AddBezier(bs);
                }
                break;
              case 4:
                {
                  last = seg.data.arc.target;
                  D2D1_ARC_SEGMENT as{
                      toD2D1(seg.data.arc.target),
                      {seg.data.arc.radius,seg.data.arc.radius},
                      0.f,
                      (seg.data.arc.sweep ? D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE : D2D1_SWEEP_DIRECTION_CLOCKWISE),
                      (seg.data.arc.large_arc ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL) 
                  };
                  s->AddArc(as);
                }
                break;
              case 5:
                {
                  last = seg.data.ellipse.target;
                  D2D1_ARC_SEGMENT as{
                    toD2D1(seg.data.ellipse.target),
                    {seg.data.ellipse.radius_x,seg.data.ellipse.radius_y},
                    seg.data.ellipse.rotation,
                    (seg.data.ellipse.sweep ? D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE : D2D1_SWEEP_DIRECTION_CLOCKWISE),
                    (seg.data.ellipse.large_arc ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL) };
                  s->AddArc(as);
                }
                break;
              case 7:
                {
                  last = seg.data.quad.point_1;
                  D2D1_QUADRATIC_BEZIER_SEGMENT qs{
                    toD2D1(seg.data.quad.control),toD2D1(seg.data.quad.point_1)
                  };
                  s->AddQuadraticBezier(qs);
                }
                break;
            }
          }

          s->EndFigure(r.closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        }

        s->Close();
        SafeRelease(&s);
        return result;
      }

      AssetImpl::FillPolygon::FillPolygon(CommandRefs& refs, const tvg::FillPolygon& fill_polygon)
        : AssetImpl::Command(refs)
        , _data(fill_polygon) {
        // 
      }

      void AssetImpl::FillPolygon::allocate() {
        // allocating _dc resources
        _brush = createBrushFromStyle(_data.prim_style);
        _path = createPathFromTVGPolygon(_data.points, true);

      }
      void AssetImpl::FillPolygon::draw() {
        if (!_path) allocate();
        if ( _path && _brush)
          _refs._dc->FillGeometry(_path, _brush, nullptr);
      }
      void AssetImpl::FillPolygon::discard()
      {
        SafeRelease(&_brush);
      }

      AssetImpl::FillPolygon::~FillPolygon()
      {
        SafeRelease(&_path);
      }

      AssetImpl::OutlineFillPolygon::OutlineFillPolygon(CommandRefs& refs, const tvg::OutlineFillPolygon& fill_polygon)
        : AssetImpl::Command(refs)
        , _data(fill_polygon) {
        // 
      }

      void AssetImpl::OutlineFillPolygon::allocate() {
        // allocating _dc resources
        _brush1 = createBrushFromStyle(_data.prim_style);
        _brush2 = createBrushFromStyle(_data.sec_style);
        _path = createPathFromTVGPolygon(_data.points, true);
      }
      void AssetImpl::OutlineFillPolygon::draw() {
        if (!_path) allocate();
        if (_path && _brush1 && _brush2)
        {
          _refs._dc->FillGeometry(_path, _brush1, nullptr);
          _refs._dc->DrawGeometry(_path, _brush2, _data.linewdith);
        }
      }
      void AssetImpl::OutlineFillPolygon::discard()
      {
        SafeRelease(&_brush2);
        SafeRelease(&_brush1);
      }

      AssetImpl::OutlineFillPolygon::~OutlineFillPolygon()
      {
        SafeRelease(&_path);
      }

      AssetImpl::FillPath::FillPath(CommandRefs& refs, const tvg::FillPath& fill_path)
        : Command(refs)
        , _data(fill_path)
      {
      }
      void AssetImpl::FillPath::draw() {
        if (!_path) allocate();
        if (_path && _brush)
          _refs._dc->FillGeometry(_path, _brush);
      }
      void AssetImpl::FillPath::discard() {
        SafeRelease(&_brush);
      }
      AssetImpl::FillPath::~FillPath() {
        SafeRelease(&_path);
      }
      void AssetImpl::FillPath::allocate() {
        _brush = createBrushFromStyle(_data.prim_style);
        _path = createPathFromTVGPath(_data.path);
      }

      AssetImpl::OutlineFillPath::OutlineFillPath(CommandRefs& refs, const tvg::OutlineFillPath& fill_path)
        : Command(refs)
        , _data(fill_path)
      {
      }
      void AssetImpl::OutlineFillPath::draw() {
        if (!_path) allocate();
        if (_path && _brush1 && _brush2) {
          _refs._dc->FillGeometry(_path, _brush1);
          _refs._dc->DrawGeometry(_path, _brush2, _data.linewdith);
        }
      }
      void AssetImpl::OutlineFillPath::discard() {
        SafeRelease(&_brush2);
        SafeRelease(&_brush1);
      }
      AssetImpl::OutlineFillPath::~OutlineFillPath() {
        SafeRelease(&_path);
      }
      void AssetImpl::OutlineFillPath::allocate() {
        _brush1 = createBrushFromStyle(_data.prim_style);
        _brush2 = createBrushFromStyle(_data.sec_style);
        _path = createPathFromTVGPath(_data.path);
      }
    }
  }
}