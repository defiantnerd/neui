#include "bitmaps.h"

#include <d2d1helper.h>
#pragma comment(lib, "d2d1") 
#include <WinRTBase.h>

namespace neui
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
    using namespace win;

    ID2D1Factory* mFactory = nullptr;

    static class COMInit
    {
    public:
      COMInit()
      {
        winrt::init_apartment();

        //auto r = CoInitialize(NULL);
        //require(r == S_OK);
        HRESULT h = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &mFactory);
        require(h == S_OK);
      }

      ~COMInit()
      {
        SafeRelease(&mFactory);
        // CoUninitialize();
        winrt::uninit_apartment();
      }

    } gComInit;

    inline ID2D1Factory* GetD2DFactory()
    {
      return mFactory;
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
    }

    Context::Context(HDC hdc, UINT w, UINT h)
      : mDC(hdc)
      , mSize{ w,h }
    {
    }

    void Context::resize(UINT width, UINT height)
    {
      mSize = D2D1::SizeU(width, height);
      if (mRenderTargetHWND && mHwnd)
      {
        mRenderTargetHWND->Resize(mSize);
      }
    }

    ID2D1RenderTarget* Context::getRenderTarget()
    {
      return (ID2D1RenderTarget*)(mRenderTargetHWND != nullptr ? (ID2D1RenderTarget*)mRenderTargetHWND : (ID2D1RenderTarget*)mRenderTargetDC);
    }

    HRESULT Context::createDeviceSpecificResources()
    {
      HRESULT hr = S_OK;

      if (!getRenderTarget())
      {
        RECT rc;
        ::GetClientRect(mHwnd, &rc);

        D2D1_SIZE_U size = D2D1::SizeU(
          rc.right - rc.left,
          rc.bottom - rc.top
        );

        if (mHwnd)
        {
          // Create a Direct2D render target.
          hr = GetD2DFactory()->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
              D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED)),
            D2D1::HwndRenderTargetProperties(mHwnd, size),
            &mRenderTargetHWND
          );
        }
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

        if (SUCCEEDED(hr))
        {
          // Create a gray brush.
          hr = getRenderTarget()->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::LightSlateGray),
            &mLightSlateGrayBrush
          );
        }
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
    }

    HRESULT Context::render()
    {
      static int blo = 0;
      blo = (blo + 1) % 5;
      static D2D1::ColorF cl[5] =
      {
        D2D1::ColorF::Orange,
        D2D1::ColorF::Red,
        D2D1::ColorF::Blue,
        D2D1::ColorF::Green,
        D2D1::ColorF::DarkCyan
      };

      HRESULT hr = createDeviceSpecificResources();

      auto target = getRenderTarget();
      if (SUCCEEDED(hr))
      {
        target->BeginDraw();
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        target->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));

        D2D1_SIZE_F rtSize = { (float)(mSize.width),(float)(mSize.height) };// target->GetSize();
        int width = mSize.width;//  static_cast<int>(rtSize.width);
        int height = mSize.height;// static_cast<int>(rtSize.height);

        mLightSlateGrayBrush->SetColor(cl[blo]);

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
        hr = target->EndDraw();
      }


      if (FAILED(hr) || hr == D2DERR_RECREATE_TARGET)
      {
        hr = S_OK;
        discardDeviceSpecificResources();
      }

      return hr;
    }
  }
}

