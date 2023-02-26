#pragma once

#include "base.h"
#include <d2d1.h>

#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>

#include <Windows.h>
#include <windowsx.h>
#include <wrl.h>
#include <wrl/client.h>

namespace wintt
{

  namespace d2d
  {
    // get rid of some typing and just import the ComPtr<T>
    template<class T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    class Factories
    {
    public:
      Factories();
      ~Factories();
      ComPtr<IDXGIFactory2> dxgi_factory;
      ComPtr<ID2D1Factory1> d2d_factory;
      ComPtr<ID3D11Device> d3d_device;
      ComPtr<IDXGIDevice> dxgi_device;
      ComPtr<ID2D1Device> d2d_device;
      ComPtr<IDWriteFactory> d2dwrite_factory;

    };
    // modern struct
    class D2D_t
    {
    public:
      void BeginDraw();
      HRESULT EndDraw();
      void Resize(HWND window, D2D_SIZE_U);
      std::shared_ptr<Factories> factory;
      ComPtr<ID2D1DeviceContext> d2d_device_ctx;
      ComPtr<IDCompositionDevice> dcomp_device;
      ComPtr<IDCompositionVisual> dcomp_visual;
      ComPtr<IDCompositionTarget> dcomp_target;
      ComPtr<IDXGISwapChain1> dxgi_swapchain;
      // ComPtr<ID2D1Bitmap1> bitmap;
      D2D1_SIZE_U s;
      ~D2D_t();
    protected:
    };

  }

  namespace d2d
  {
    // new interface
    std::unique_ptr<D2D_t> CreateDirect2DContextForWindow(HWND window);

    class Context
    {
    public:
      Context(HWND hwnd, UINT w, UINT h);
      Context(HDC hdc, UINT w, UINT h);
      ~Context();
      HRESULT createDeviceSpecificResources();
      void discardDeviceSpecificResources();
      HRESULT render();
      void resize(UINT width, UINT height);
    protected:
      std::unique_ptr<D2D_t> context;

      ID2D1RenderTarget* getRenderTarget();
      ID2D1HwndRenderTarget* mRenderTargetHWND = nullptr;
      ID2D1DCRenderTarget* mRenderTargetDC = nullptr;
      HWND mHwnd = 0;
      HDC mDC = 0;
      D2D_SIZE_U mSize;
      // todo: remove later
      ID2D1SolidColorBrush* mLightSlateGrayBrush = nullptr;
      ID2D1SolidColorBrush* mCornflowerBlueBrush = nullptr;
      ID2D1SolidColorBrush* mBlackBrush = nullptr;
    };
  }
}