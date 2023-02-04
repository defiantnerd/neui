#include "wic.h"
#include <d2d1_2.h>
#include <Shlwapi.h>

#pragma comment(lib, "windowscodecs")
#pragma comment(lib, "shlwapi")

namespace neui
{
  using namespace D2D1;
  using namespace Microsoft::WRL;

  /*
  * returns a D2D1BitmapRef from a byte stream
  */
  D2D1BitmapRef WIC::createD2D1BitmapFromBuffer(const uint8_t* buffer, uint32_t bufferSize) const
  {
    D2D1BitmapRef d2d1Bitmap = nullptr;

    // creating a bytestream, so the decoder can read from that IStream
    auto comStream = Microsoft::WRL::ComPtr<IStream>(SHCreateMemStream(reinterpret_cast<const BYTE*>(buffer), bufferSize));
    if (comStream == nullptr) return d2d1Bitmap;

    // creating a decoder, that can read a bytestream
    ComPtr<IWICBitmapDecoder> decoder;
    if (wicFactory.CreateDecoderFromStream(comStream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
      decoder.GetAddressOf()) != S_OK)
    {
      return d2d1Bitmap;
    }

    // create a framedecoder, that decodes a frame FROM a decoded stream
    ComPtr<IWICBitmapFrameDecode> source;
    if (decoder->GetFrame(0, source.GetAddressOf()) != S_OK)
    {
      return d2d1Bitmap;
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> image;
    if (wicFactory.CreateFormatConverter(image.GetAddressOf()) != S_OK)
    {
      return d2d1Bitmap;
    }

    if (image->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
      WICBitmapPaletteTypeMedianCut) != S_OK)
    {
      return d2d1Bitmap;
    }

    deviceContext.CreateBitmapFromWicBitmap(image.Get(), d2d1Bitmap.GetAddressOf());
    return d2d1Bitmap;
  }
}