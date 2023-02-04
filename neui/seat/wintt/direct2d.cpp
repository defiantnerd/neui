#include "direct2d.h"
#include <d2d1helper.h>
#include <dwrite.h>
// #include <d2d1effects_2.h>

#pragma comment(lib, "d2d1") 
#pragma comment(lib, "d3d11")
#pragma comment(lib,"dcomp")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "dwrite")
// #pragma comment(lib, "dxguid")

#include <dxgidebug.h>

namespace wintt
{  
  template<class Interface>
  inline void SafeRelease(Interface** ppInterfaceToRelease)
  {
    if (*ppInterfaceToRelease != NULL)
    {
      (*ppInterfaceToRelease)->Release();
      (*ppInterfaceToRelease) = NULL;
    }
  }

  namespace d2d
  {

    std::shared_ptr<Factories> gFactories;

    static class COMInit
    {
    public:
      COMInit()
      {
        auto r = CoInitialize(NULL);
        require(r == S_OK);
        gFactories = std::make_shared<Factories>();
      }

      ~COMInit()
      {
        gFactories.reset(); 
        CoUninitialize();
      }

    } gComInit;

    Factories::Factories()
    {
      static const D3D_FEATURE_LEVEL levels[] =
      {
          D3D_FEATURE_LEVEL_12_1,
          D3D_FEATURE_LEVEL_12_0,
          D3D_FEATURE_LEVEL_11_1,
          D3D_FEATURE_LEVEL_11_0,
          D3D_FEATURE_LEVEL_10_1,
          D3D_FEATURE_LEVEL_10_0
      };

      com_require(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT |
        D3D11_CREATE_DEVICE_DEBUG,
        levels, sizeof(levels) / sizeof(D3D_FEATURE_LEVEL),
        D3D11_SDK_VERSION, &d3d_device, NULL, NULL));
      
      // check for the interface
      com_require(d3d_device->QueryInterface(dxgi_device.ReleaseAndGetAddressOf()));

      com_require(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG,
        __uuidof(dxgi_factory), &dxgi_factory));

      com_require(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(d2d_factory), nullptr, &d2d_factory));

      com_require(d2d_factory->CreateDevice(dxgi_device.Get(), &d2d_device));

      com_require(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(d2dwrite_factory), &d2dwrite_factory));
    }

    Factories::~Factories()
    {
      d2d_device.Reset();
      dxgi_device.Reset();
      d3d_device.Reset();
      dxgi_factory.Reset();
      d2d_factory.Reset();
    }

    void setBackBuffer(D2D_t* context)
    {
      ComPtr<IDXGISurface> surface;
      ComPtr<ID2D1Bitmap1> bitmap;

      com_require(context->dxgi_swapchain->GetBuffer(0, __uuidof(surface), (void**)surface.ReleaseAndGetAddressOf()));
      if (surface)
      {
        com_require(context->d2d_device_ctx->CreateBitmapFromDxgiSurface(surface.Get(), NULL, bitmap.ReleaseAndGetAddressOf()));
      }

      if (bitmap)
      {
        // context->bitmap = bitmap;
        context->d2d_device_ctx->SetTarget(bitmap.Get());
      }
    }

    D2D_t::~D2D_t()
    {
      
      dxgi_swapchain.Reset();
      dcomp_target.Reset();
      dcomp_visual.Reset();
      dcomp_device.Reset();
      d2d_device_ctx.Reset();
    }

    void CreateCompositionTarget(HWND window, D2D_t* context)
    {
      DXGI_SWAP_CHAIN_DESC1 desc;
      RECT r;

      require(GetClientRect(window, &r));

      // You must specify the DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL value in the SwapEffect member of DXGI_SWAP_CHAIN_DESC1 because 
      // CreateSwapChainForComposition supports only flip presentation model.
      // https://docs.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforcomposition

      desc.Width = r.right - r.left; /* width of client area */
      desc.Height = r.bottom - r.top; /* height of client area */
      desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.Stereo = FALSE;
      desc.SampleDesc.Count = 1;
      desc.SampleDesc.Quality = 0;
      desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      desc.BufferCount = 2;
      desc.Scaling =  DXGI_SCALING_STRETCH;
      desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
      desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
      desc.Flags = 0;

      com_require(gFactories->dxgi_factory->CreateSwapChainForComposition(gFactories->dxgi_device.Get(), &desc, NULL, context->dxgi_swapchain.ReleaseAndGetAddressOf()));

      setBackBuffer(context);
    }

    std::unique_ptr<D2D_t> CreateDirect2DContextForWindow(HWND window)
    {
      std::unique_ptr<D2D_t> result = std::make_unique<D2D_t>();
      result->factory = gFactories;

      com_require(gFactories->d2d_device->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        &result->d2d_device_ctx)
      );

      com_require(DCompositionCreateDevice(gFactories->dxgi_device.Get(),
        __uuidof(result->dcomp_device), (void**)result->dcomp_device.ReleaseAndGetAddressOf()));


      com_require(result->dcomp_device->CreateVisual(result->dcomp_visual.ReleaseAndGetAddressOf()));

      com_require(result->dcomp_device->CreateTargetForHwnd(window, FALSE, result->dcomp_target.ReleaseAndGetAddressOf()));

      CreateCompositionTarget(window, result.get());

      return std::move(result);
    }

    void D2D_t::BeginDraw()
    {      
      this->d2d_device_ctx->BeginDraw();
    }

    HRESULT D2D_t::EndDraw()
    {

      auto hr = this->d2d_device_ctx->EndDraw(NULL, NULL);

      dxgi_swapchain->Present(1, 0); // or 1,0
      dcomp_visual->SetContent(this->dxgi_swapchain.Get());
      dcomp_target->SetRoot(this->dcomp_visual.Get());
      dcomp_device->Commit();

      return hr;
    }

    void D2D_t::Resize(HWND hwnd, D2D_SIZE_U size)
    {
      d2d::CreateCompositionTarget(hwnd, this);
      if (dxgi_swapchain)
      {
        /*this->dcomp_visual->SetClip(D2D_RECT_F{ 0.f, 0.f, (float) size.width, (float)size.height });
        dxgi_swapchain->ResizeBuffers(2, size.width, size.height, DXGI_FORMAT_UNKNOWN, 0);*/

        // dxgi_swapchain->ResizeTarget(&desc);


        // com_require(dxgi_swapchain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0));
      }
    }

    //
    //class Context
    //{
    //public:
    //  Context(HWND hwnd, UINT w, UINT h);
    //  HRESULT createDeviceSpecificResources();
    //  void discardDeviceSpecificResources();
    //  HRESULT render();
    //  void resize(UINT width, UINT height);
    //protected:
    //  ID2D1HwndRenderTarget* mRenderTarget = nullptr;
    //  HWND mHwnd;
    //  D2D_SIZE_U mSize;
    //  // todo: remove later
    //  ID2D1SolidColorBrush* mLightSlateGrayBrush = nullptr;
    //  ID2D1SolidColorBrush* mCornflowerBlueBrush = nullptr;
    //};

    Context::Context(HWND hwnd, UINT w, UINT h)
      : mHwnd(hwnd)
      , mSize{ w,h }
    {
      // todo: check if a repeated call to getparent retrieves a HWND that
      // is already present in the resource management so bitmaps and other 
      // resources can be shared
      HWND parent = hwnd;
      do
      {
        HWND next = GetParent(parent);
        if (next)
        {
          parent = next;
        }
        else
        {
          break;
        }
      } while (true);
      // check if parent is in the resource management
      if (parent)
      {
        // 
      }
      createDeviceSpecificResources();
    }

    Context::Context(HDC hdc, UINT w, UINT h)
      : mDC(hdc)
      , mSize{ w,h }
    {
    }

    Context::~Context()
    {
      discardDeviceSpecificResources();
      this->context.reset();
    }

    void Context::resize(UINT width, UINT height)
    {
      mSize = D2D1::SizeU(width, height);
      this->context->Resize(mHwnd, mSize);
      //if (mRenderTargetHWND && mHwnd)
      //{
      //  mRenderTargetHWND->Resize(mSize);
      //}
    }

    ID2D1RenderTarget* Context::getRenderTarget()
    {
      if ( context )
        return context->d2d_device_ctx.Get();
      return nullptr;
      // return (ID2D1RenderTarget*)(mRenderTargetHWND != nullptr ? (ID2D1RenderTarget*)mRenderTargetHWND : (ID2D1RenderTarget*)mRenderTargetDC);
    }

    HRESULT Context::createDeviceSpecificResources()
    {
      HRESULT hr = S_OK;

      if (!getRenderTarget())
      {
        RECT rc;
        ::GetClientRect(mHwnd,&rc);

        D2D1_SIZE_U size = D2D1::SizeU(
          rc.right - rc.left,
          rc.bottom - rc.top
        );

        if (mHwnd)
        {
          this->context = CreateDirect2DContextForWindow(mHwnd);
#if 0
          // Create a Direct2D render target.
          hr = GetD2DFactory()->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
              D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED)),
            D2D1::HwndRenderTargetProperties(mHwnd, size),
            &mRenderTargetHWND
          );
#endif
        }
#if 0
        if (mDC)
        {

          D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(
              DXGI_FORMAT_B8G8R8A8_UNORM,
              D2D1_ALPHA_MODE_PREMULTIPLIED),
            0,
            0,
            D2D1_RENDER_TARGET_USAGE_NONE,
            D2D1_FEATURE_LEVEL_DEFAULT
          );

          hr = GetD2DFactory()->CreateDCRenderTarget(
            &props,
            &mRenderTargetDC
          );
        }
#endif
        if (SUCCEEDED(hr))
        {
          // Create a gray brush.
          hr = getRenderTarget()->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::LightSlateGray),
            &mLightSlateGrayBrush
          );
        }
        com_require(getRenderTarget()->CreateSolidColorBrush(
          D2D1::ColorF(D2D1::ColorF::Black),&mBlackBrush
        ));
        if (SUCCEEDED(hr))
        {
          // Create a blue brush.
          hr = getRenderTarget()->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::CornflowerBlue),
            &mCornflowerBlueBrush
          );
        }
      }
      return hr;

    }
    void Context::discardDeviceSpecificResources()
    {
      SafeRelease(&mRenderTargetHWND);
      SafeRelease(&mRenderTargetDC);
      SafeRelease(&mLightSlateGrayBrush);
      SafeRelease(&mCornflowerBlueBrush);
      SafeRelease(&mBlackBrush);
    }

    HRESULT Context::render()
    {
      static int blo = 0;
      // blo = (blo + 1) % 5;
      static D2D1::ColorF cl[5] =
      {        
        D2D1::ColorF::Orange,
        D2D1::ColorF::Red,
        D2D1::ColorF::Blue,
        D2D1::ColorF::Green,
        D2D1::ColorF::DarkCyan
      };

      D2D1::ColorF transparent{ 0.f,0.f,0.f,0.f };
      
      HRESULT hr = S_OK; //  createDeviceSpecificResources();

      // return 0;
      auto ct = this->context->d2d_device_ctx.Get();
      this->context->BeginDraw();
      ct->Clear(&transparent);
      ComPtr<ID2D1Effect> effect;

      //const IID effectid = CLSID_D2D1Tint;
      //ct->CreateEffect(effectid, effect.ReleaseAndGetAddressOf());
      //effect->SetValue(D2D1_TINT_PROP_COLOR, D2D1_VECTOR_4F{ 1.f, 1.f, 0.f, 0.f });
            
      // std::cout << "render 6" << std::endl;

      ID2D1SolidColorBrush* brush;

      if (S_OK == ct->CreateSolidColorBrush({ 1.0f,0.2f,0.2f,0.3f }, &brush))
      {
        ct->DrawRoundedRectangle({ {20.0f,20.0f,160.0f,160.0f},15.f,15.f }, brush);
        // std::cout << "render 7" << std::endl;
      }
      brush->Release();
      
      if (SUCCEEDED(hr))
      {
        // this->context->BeginDraw();
        auto target = this->context->d2d_device_ctx;
        // target->BeginDraw();
        IDWriteTextFormat* format = nullptr;
        context->factory->d2dwrite_factory->CreateTextFormat(_T("Segoe"),
          NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.f*2, L"en-us", &format);
        if (format)
        {
          target->DrawText(_T("Lalula"), 6, format, { 0.f,0.f,100.f,20.f }, mBlackBrush);
        }
        format->Release();

        target->SetTransform(D2D1::Matrix3x2F::Identity());
        // target->Clear(D2D1::ColorF(1.f, 0.f, 0.f, 0.5f));
        D2D1_SIZE_F rtSize = { (float) mSize.width, (float) mSize.height }; //  target->GetSize(); //  { (float)(mSize.width), (float)(mSize.height) };// target->GetSize();
        int width = mSize.width;//  static_cast<int>(rtSize.width);
        int height = mSize.height;// static_cast<int>(rtSize.height);

        mLightSlateGrayBrush->SetColor(cl[blo ]);

        // Draw horizontal lines
        for (int x = 0; x < width; x += 10)
        {
          target->DrawLine(
            D2D1::Point2F(static_cast<FLOAT>(x), 0.0f),
            D2D1::Point2F(static_cast<FLOAT>(x), rtSize.height),
            mLightSlateGrayBrush,
            0.5f
          );
        }

        // Draw vertical lines
        for (int y = 0; y < height; y += 10)
        {
          target->DrawLine(
            D2D1::Point2F(0.0f, static_cast<FLOAT>(y)),
            D2D1::Point2F(rtSize.width, static_cast<FLOAT>(y)),
            mLightSlateGrayBrush,
            0.5f
          );
        }

        // Draw two rectangles.
        D2D1_RECT_F rectangle1 = D2D1::RectF(
          rtSize.width / 2 - 50.0f,
          rtSize.height / 2 - 50.0f,
          rtSize.width / 2 + 50.0f,
          rtSize.height / 2 + 50.0f
        );

        D2D1_RECT_F rectangle2 = D2D1::RectF(
          rtSize.width / 2 - 100.0f,
          rtSize.height / 2 - 100.0f,
          rtSize.width / 2 + 100.0f,
          rtSize.height / 2 + 100.0f
        );

        // Draw a filled rectangle.
        target->FillRectangle(&rectangle1, mLightSlateGrayBrush);

        // Draw the outline of a rectangle.
        target->DrawRectangle(&rectangle2, mCornflowerBlueBrush);
        // hr = target->EndDraw();
        // target->EndDraw();
        // hr = this->context->EndDraw();
      }
      hr = this->context->EndDraw();


      if (FAILED(hr) || hr == D2DERR_RECREATE_TARGET)
      {
        hr = S_OK;
        discardDeviceSpecificResources();
      }

      return hr;
    }
  }
}

