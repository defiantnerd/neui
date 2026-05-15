#include "ImagePool.h"
#include "direct2d.h"

namespace neui
{
  namespace win {

    neui::
    ImagePool::ImagePool(IWICImagingFactory* wicFactory)
      : _wicFactory(wicFactory) {
    }

    bool ImagePool::loadPNGFromFile(const std::wstring& identifier, const std::wstring& filePath) {
      ComPtr<IWICBitmapDecoder> decoder;
      if (FAILED(_wicFactory->CreateDecoderFromFilename(filePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
        return false;

      ComPtr<IWICBitmapFrameDecode> frame;
      decoder->GetFrame(0, &frame);

      ComPtr<IWICBitmap> wicBitmap;
      _wicFactory->CreateBitmapFromSource(frame.Get(), WICBitmapCacheOnLoad, &wicBitmap);

      _imageMap[identifier] = wicBitmap;
      return true;
    }

    bool ImagePool::loadPNGFromMemory(const std::wstring& identifier, const void* buffer, UINT size) {
      ComPtr<IWICStream> stream;
      _wicFactory->CreateStream(&stream);
      stream->InitializeFromMemory((BYTE*)buffer, size);

      ComPtr<IWICBitmapDecoder> decoder;
      _wicFactory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);

      ComPtr<IWICBitmapFrameDecode> frame;
      decoder->GetFrame(0, &frame);

      ComPtr<IWICBitmap> wicBitmap;
      _wicFactory->CreateBitmapFromSource(frame.Get(), WICBitmapCacheOnLoad, &wicBitmap);

      _imageMap[identifier] = wicBitmap;
      return true;
    }

    bool ImagePool::ConvertAndCreateLockedBitmap(ID2D1DeviceContext* deviceContext, const std::wstring& identifier, ID2D1Bitmap** outBitmap) {
      if (!deviceContext || !outBitmap) return false;

      auto it = _imageMap.find(identifier);
      if (it == _imageMap.end()) return false;

      ComPtr<IWICBitmap> wicBitmap = it->second;
      WICRect lockRect = { 0, 0, 0, 0 };
      wicBitmap->GetSize(reinterpret_cast<UINT*>(&lockRect.Width), reinterpret_cast<UINT*>(&lockRect.Height));

      ComPtr<IWICBitmapLock> bitmapLock;
      if (FAILED(wicBitmap->Lock(&lockRect, WICLockRead, &bitmapLock)))
        return false;

      UINT bufferSize = 0;
      BYTE* pixelData = nullptr;
      bitmapLock->GetDataPointer(&bufferSize, &pixelData);

      D2D1_BITMAP_PROPERTIES properties = {};
      properties.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
      properties.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
      properties.dpiX = 96.0f;
      properties.dpiY = 96.0f;

      return SUCCEEDED(deviceContext->CreateBitmap(D2D1::SizeU(lockRect.Width, lockRect.Height), pixelData, lockRect.Width * 4, &properties, outBitmap));
    }

  } // namespace win
} // namespace neui


#if 0
#include "wic.h"

namespace neui
{
  using namespace win;

  bool ImagePool::loadPNGFromFile(const std::wstring& identifier, const std::wstring& filePath) {
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(_wicFactory->CreateDecoderFromFilename(filePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
      return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    decoder->GetFrame(0, &frame);

    ComPtr<IWICFormatConverter> converter;
    _wicFactory->CreateFormatConverter(&converter);
    converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom);

    _imageMap[identifier] = converter;
    return true;
  }

  // PNG aus Speicherbereich laden
  bool ImagePool::loadPNGFromMemory(const std::wstring& identifier, const void* buffer, UINT size) {
    ComPtr<IWICStream> stream;
    _wicFactory->CreateStream(&stream);
    stream->InitializeFromMemory((BYTE*)buffer, size);

    ComPtr<IWICBitmapDecoder> decoder;
    _wicFactory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);

    ComPtr<IWICBitmapFrameDecode> frame;
    decoder->GetFrame(0, &frame);

    ComPtr<IWICFormatConverter> converter;
    _wicFactory->CreateFormatConverter(&converter);
    converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom);

    _imageMap[identifier] = converter;
    return true;
  }

  ID2D1Bitmap* ImagePool::getBitmap(ID2D1DeviceContext* deviceContext, const std::wstring& identifier) {
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
#if 0
    // ID2D1Bitmap auf Anfrage aus einem DeviceContext erstellen
    ID2D1Bitmap* ImagePool::getBitmap(ID2D1DeviceContext * deviceContext, const std::wstring & identifier) {
      auto it = _imageMap.find(identifier);
      if (it == _imageMap.end()) return nullptr; // Falls das Bild nicht existiert

      ComPtr<ID2D1Bitmap> d2dBitmap;
      if (FAILED(deviceContext->CreateBitmapFromWicBitmap(it->second.Get(), nullptr, &d2dBitmap)))
        return nullptr;

      return d2dBitmap.Detach(); // Übergibt den Zeiger ohne weitere Speicherverwaltung
    }
#endif

  }
#endif