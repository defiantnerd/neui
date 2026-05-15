#pragma once

#include <wincodec.h>
#include <d2d1.h>
#include <unordered_map>
#include <string>
#include <memory>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace neui {
  namespace win {

    class ImagePool {
    public:
      explicit ImagePool(IWICImagingFactory* wicFactory);

      bool loadPNGFromFile(const std::wstring& identifier, const std::wstring& filePath);
      bool loadPNGFromMemory(const std::wstring& identifier, const void* buffer, UINT size);
      bool ConvertAndCreateLockedBitmap(ID2D1DeviceContext* deviceContext, const std::wstring& identifier, ID2D1Bitmap** outBitmap);

    private:
      IWICImagingFactory* _wicFactory;
      std::unordered_map<std::wstring, ComPtr<IWICBitmap>> _imageMap;
    };

  } // namespace win
}



#if 0
#include "base.h"
#include "../resource.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <d2d1.h>
#include <unordered_map>
#include <string>
#include <memory>

using Microsoft::WRL::ComPtr;

namespace neui
{
	using namespace win;

  class ImagePool {
  public:
    ImagePool(IWICImagingFactory* wicFactory)
      : _wicFactory(wicFactory) {
    }

    // PNG aus Datei laden und speichern
    bool loadPNGFromFile(const std::wstring& identifier, const std::wstring& filePath);

    // PNG aus Speicherbereich laden
    bool loadPNGFromMemory(const std::wstring& identifier, const void* buffer, UINT size);

    // ID2D1Bitmap auf Anfrage aus einem DeviceContext erstellen
    ID2D1Bitmap* getBitmap(ID2D1DeviceContext* deviceContext, const std::wstring& identifier);

#if 0
    // Erstelle eine Shared Bitmap aus einem Identifier und DeviceContext
    ID2D1Bitmap* getBitmap(ID2D1DeviceContext* deviceContext, const std::wstring& identifier) {
      auto it = _imageMap.find(identifier);
      if (it == _imageMap.end()) return nullptr; // Falls kein Bild vorhanden ist

      ComPtr<IWICBitmapLock> bitmapLock;
      ComPtr<IWICBitmap> wicBitmap;
      _wicFactory->CreateBitmapFromSource(it->second.Get(), WICBitmapCacheOnDemand, &wicBitmap);

      WICRect lockRect = { 0, 0, 0, 0 };
      wicBitmap->GetSize(reinterpret_cast<UINT*>(&lockRect.Width), reinterpret_cast<UINT*>(&lockRect.Height));

      // Sperre das Bild für direkten Datenzugriff
      if (FAILED(wicBitmap->Lock(&lockRect, WICLockRead, &bitmapLock)))
        return nullptr;

      // Erstelle eine Shared Bitmap für Direct2D
      ComPtr<ID2D1Bitmap> sharedBitmap;
      if (FAILED(deviceContext->CreateSharedBitmap(__uuidof(IWICBitmapLock), bitmapLock.Get(), nullptr, &sharedBitmap)))
        return nullptr;

      return sharedBitmap.Detach(); // Übergibt den Zeiger ohne Speicherverwaltung
    }
#endif



  private:
    IWICImagingFactory* _wicFactory;
    std::unordered_map<std::wstring, ComPtr<IWICFormatConverter>> _imageMap;
  };



}
#endif