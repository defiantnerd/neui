#pragma once

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincodec.h>
#include <shlwapi.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "../../shared/win32/image_loader_win32.h"   // wic_factory_w32

#pragma comment(lib, "Windowscodecs")
#pragma comment(lib, "shlwapi")

// PNG <-> CF_DIBV5 conversion for Win32 clipboard / DnD interop.
//
// PNG bytes carry their own headers; native Windows apps (Explorer, Paint,
// Outlook) speak CF_DIBV5 / CF_DIB (BMP-style headers + pixel data). To
// bridge, we decode PNG via WIC to a BGRA8 buffer and pack a
// BITMAPV5HEADER + pixels; reverse direction parses the BITMAPV5HEADER
// pixel data and re-encodes through WIC's PNG encoder.
//
// Limitations:
//   - DIB decode handles 32bpp BGRA/BGRX both top-down (negative biHeight)
//     and bottom-up. 24bpp / 16bpp / palette modes are out of scope for
//     v1 (PNG round-trip is the primary client; native apps producing
//     non-32bpp typically advertise CF_DIB rather than CF_DIBV5).
//   - The pre-multiplication state of the clipboard DIB is ambiguous in
//     practice (Win32 has no standardized bit for this). We treat the
//     bytes as straight (unpremultiplied) BGRA on the wire, which matches
//     what Paint / Snip & Sketch produce; the WIC PNG encoder also wants
//     straight alpha.

namespace neui_detail
{
  // ---- PNG bytes -> BGRA8 -------------------------------------------------

  // Returns a heap-allocated BGRA8 buffer (caller releases via delete[])
  // and writes the pixel dimensions; null on failure. Uses straight
  // (non-premultiplied) alpha so the same buffer round-trips through PNG
  // encoders that expect straight alpha.
  inline uint8_t* png_bytes_to_bgra8_w32(const uint8_t* png, uint32_t png_size,
                                          uint32_t* width_out,
                                          uint32_t* height_out)
  {
    if (!png || png_size == 0 || !width_out || !height_out) return nullptr;
    IWICImagingFactory* wic = wic_factory_w32();
    if (!wic) return nullptr;

    IStream* stream = SHCreateMemStream(png, png_size);
    if (!stream) return nullptr;

    IWICBitmapDecoder*     decoder   = nullptr;
    IWICBitmapFrameDecode* frame     = nullptr;
    IWICFormatConverter*   converter = nullptr;
    uint8_t*               result    = nullptr;

    HRESULT hr = wic->CreateDecoderFromStream(
      stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) goto cleanup;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) goto cleanup;
    hr = wic->CreateFormatConverter(&converter);
    if (FAILED(hr)) goto cleanup;
    hr = converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                 WICBitmapDitherTypeNone, nullptr, 0.0,
                                 WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) goto cleanup;
    {
      UINT w = 0, h = 0;
      converter->GetSize(&w, &h);
      if (w == 0 || h == 0) goto cleanup;
      uint32_t stride = w * 4;
      uint32_t size   = stride * h;
      result = new uint8_t[size];
      hr = converter->CopyPixels(nullptr, stride, size, result);
      if (FAILED(hr)) { delete[] result; result = nullptr; goto cleanup; }
      *width_out  = w;
      *height_out = h;
    }
  cleanup:
    if (converter) converter->Release();
    if (frame)     frame->Release();
    if (decoder)   decoder->Release();
    stream->Release();
    return result;
  }

  // ---- BGRA8 -> PNG bytes -------------------------------------------------

  inline std::vector<uint8_t>
  bgra8_to_png_bytes_w32(const uint8_t* bgra8, uint32_t width, uint32_t height)
  {
    std::vector<uint8_t> out;
    if (!bgra8 || width == 0 || height == 0) return out;
    IWICImagingFactory* wic = wic_factory_w32();
    if (!wic) return out;

    IStream*           stream  = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (FAILED(hr) || !stream) goto cleanup;

    hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) goto cleanup;
    hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) goto cleanup;
    hr = encoder->CreateNewFrame(&frame, nullptr);
    if (FAILED(hr)) goto cleanup;
    hr = frame->Initialize(nullptr);
    if (FAILED(hr)) goto cleanup;
    hr = frame->SetSize(width, height);
    if (FAILED(hr)) goto cleanup;
    {
      WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
      hr = frame->SetPixelFormat(&fmt);
      if (FAILED(hr)) goto cleanup;
      uint32_t stride = width * 4;
      uint32_t size   = stride * height;
      hr = frame->WritePixels(height, stride, size,
                                const_cast<BYTE*>(bgra8));
      if (FAILED(hr)) goto cleanup;
    }
    hr = frame->Commit();
    if (FAILED(hr)) goto cleanup;
    hr = encoder->Commit();
    if (FAILED(hr)) goto cleanup;

    // Read the encoded bytes out of the IStream.
    {
      LARGE_INTEGER zero = {};
      stream->Seek(zero, STREAM_SEEK_SET, nullptr);
      STATSTG st = {};
      if (SUCCEEDED(stream->Stat(&st, STATFLAG_NONAME))) {
        uint64_t total = st.cbSize.QuadPart;
        out.resize(static_cast<size_t>(total));
        ULONG n = 0;
        stream->Read(out.data(), static_cast<ULONG>(total), &n);
        if (n != total) out.clear();
      }
    }

  cleanup:
    if (frame)   frame->Release();
    if (encoder) encoder->Release();
    if (stream)  stream->Release();
    return out;
  }

  // ---- BGRA8 -> CF_DIBV5 bytes -------------------------------------------

  // Build a BITMAPV5HEADER + top-down BGRA8 pixel block ready to drop into
  // an HGLOBAL for SetClipboardData(CF_DIBV5, ...).
  inline std::vector<uint8_t>
  bgra8_to_dibv5_bytes_w32(const uint8_t* bgra8, uint32_t width, uint32_t height)
  {
    std::vector<uint8_t> out;
    if (!bgra8 || width == 0 || height == 0) return out;
    uint32_t stride = width * 4;
    uint32_t pixels_size = stride * height;
    out.assign(sizeof(BITMAPV5HEADER) + pixels_size, 0);
    auto* hdr = reinterpret_cast<BITMAPV5HEADER*>(out.data());
    hdr->bV5Size        = sizeof(BITMAPV5HEADER);
    hdr->bV5Width       = static_cast<LONG>(width);
    hdr->bV5Height      = -static_cast<LONG>(height);  // top-down
    hdr->bV5Planes      = 1;
    hdr->bV5BitCount    = 32;
    hdr->bV5Compression = BI_BITFIELDS;
    hdr->bV5SizeImage   = pixels_size;
    hdr->bV5RedMask     = 0x00FF0000;
    hdr->bV5GreenMask   = 0x0000FF00;
    hdr->bV5BlueMask    = 0x000000FF;
    hdr->bV5AlphaMask   = 0xFF000000;
    hdr->bV5CSType      = LCS_sRGB;
    hdr->bV5Intent      = LCS_GM_GRAPHICS;
    std::memcpy(out.data() + sizeof(BITMAPV5HEADER), bgra8, pixels_size);
    return out;
  }

  // ---- CF_DIBV5 / CF_DIB bytes -> BGRA8 ----------------------------------

  // Parse a clipboard DIB payload (CF_DIB or CF_DIBV5 - both start with
  // a BITMAPINFOHEADER-compatible prefix that names the actual size).
  // Returns a heap-allocated BGRA8 buffer or null on failure / unsupported
  // pixel layout. Only 32bpp BGRA/BGRX, BI_RGB or BI_BITFIELDS, are
  // recognised - which covers the formats Paint / Snipping Tool / browser
  // copy produce. Other depths fall through (clipboard reader will skip
  // the format instead of producing garbage).
  inline uint8_t* dib_bytes_to_bgra8_w32(const uint8_t* dib, uint32_t dib_size,
                                          uint32_t* width_out,
                                          uint32_t* height_out)
  {
    if (!dib || dib_size < sizeof(BITMAPINFOHEADER) ||
        !width_out || !height_out) return nullptr;

    auto* bih = reinterpret_cast<const BITMAPINFOHEADER*>(dib);
    if (bih->biSize < sizeof(BITMAPINFOHEADER)) return nullptr;
    if (bih->biBitCount != 32) return nullptr;
    if (bih->biCompression != BI_RGB && bih->biCompression != BI_BITFIELDS)
      return nullptr;

    uint32_t header_size = bih->biSize;
    // BI_BITFIELDS appends three DWORD masks AFTER the header for V1;
    // for V4/V5 the masks live inside the header itself.
    uint32_t masks_size = 0;
    if (bih->biCompression == BI_BITFIELDS &&
        bih->biSize == sizeof(BITMAPINFOHEADER)) {
      masks_size = 3 * sizeof(DWORD);
    }
    if (dib_size < header_size + masks_size) return nullptr;

    LONG width  = bih->biWidth;
    LONG height = bih->biHeight;
    if (width <= 0 || height == 0) return nullptr;
    bool top_down = (height < 0);
    if (top_down) height = -height;

    uint32_t stride = static_cast<uint32_t>(width) * 4;
    uint32_t pixels_size = stride * static_cast<uint32_t>(height);
    const uint8_t* src = dib + header_size + masks_size;
    if (dib_size < header_size + masks_size + pixels_size) return nullptr;

    uint8_t* result = new uint8_t[pixels_size];
    if (top_down) {
      std::memcpy(result, src, pixels_size);
    } else {
      // Bottom-up: copy rows in reverse so the output is top-down BGRA.
      for (LONG y = 0; y < height; ++y) {
        std::memcpy(result + y * stride,
                     src    + (height - 1 - y) * stride,
                     stride);
      }
    }
    *width_out  = static_cast<uint32_t>(width);
    *height_out = static_cast<uint32_t>(height);
    return result;
  }

  // ---- Convenience: PNG bytes <-> CF_DIBV5 bytes -------------------------

  inline std::vector<uint8_t>
  png_bytes_to_dibv5_bytes_w32(const uint8_t* png, uint32_t png_size)
  {
    uint32_t w = 0, h = 0;
    uint8_t* bgra = png_bytes_to_bgra8_w32(png, png_size, &w, &h);
    if (!bgra) return {};
    auto out = bgra8_to_dibv5_bytes_w32(bgra, w, h);
    delete[] bgra;
    return out;
  }

  inline std::vector<uint8_t>
  dib_bytes_to_png_bytes_w32(const uint8_t* dib, uint32_t dib_size)
  {
    uint32_t w = 0, h = 0;
    uint8_t* bgra = dib_bytes_to_bgra8_w32(dib, dib_size, &w, &h);
    if (!bgra) return {};
    auto out = bgra8_to_png_bytes_w32(bgra, w, h);
    delete[] bgra;
    return out;
  }

} // namespace neui_detail

#endif // _WIN32
