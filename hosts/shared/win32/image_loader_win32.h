#pragma once

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>
#include <shlwapi.h>

#include <cstdint>
#include <string>

#pragma comment(lib, "Windowscodecs")
#pragma comment(lib, "shlwapi")

// Win32 image-loading helpers shared by both the xpl host's Windows
// platform layer (`hosts/crossplatform/platform_win32.cpp`) and the
// native Win32 host (`hosts/win32/widgets.cpp`). Both decode via WIC
// into a BGRA8-premultiplied buffer matching the d2d backend.
//
// Resource-first: tries FindResourceW(L"\"path\"", L"PNG") on the
// process EXE module before falling back to disk. Quirk: rc.exe stores
// quoted resource names in `.rc` files with the literal `"` characters
// baked into the name, so the lookup string is wrapped in '"' here to
// match. FindResource is case-insensitive on string names, so the
// rc-side uppercasing is transparent.
//
// CONVENTION: include from C++ TUs only.

namespace neui_detail
{
  inline IWICImagingFactory* wic_factory_w32()
  {
    static IWICImagingFactory* fac    = nullptr;
    static bool                tried  = false;
    if (!fac && !tried) {
      tried = true;
      HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                     CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&fac));
      if (FAILED(hr)) fac = nullptr;
    }
    return fac;
  }

  // Shared tail: run an initialised WIC decoder's first frame through a format
  // converter into a new[]-allocated buffer. Releases nothing the caller owns.
  inline uint8_t* wic_decode_frame_w32(IWICImagingFactory* wic,
                                       IWICBitmapDecoder* decoder,
                                       const WICPixelFormatGUID& pixel_format,
                                       uint32_t* width_out,
                                       uint32_t* height_out)
  {
    IWICBitmapFrameDecode* frame     = nullptr;
    IWICFormatConverter*   converter = nullptr;
    uint8_t*               result    = nullptr;

    HRESULT hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) goto cleanup;

    hr = wic->CreateFormatConverter(&converter);
    if (FAILED(hr)) goto cleanup;

    hr = converter->Initialize(frame, pixel_format,
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

      if (width_out)  *width_out  = w;
      if (height_out) *height_out = h;
    }

  cleanup:
    if (converter) converter->Release();
    if (frame)     frame->Release();
    return result;
  }

  // Decode encoded image bytes already in memory - the client resource provider
  // path (NEUI_API_RESOURCE_CLIENT) has no path to hand over. Also the engine
  // behind the embedded-resource branch of load_image_bgra8_w32 below.
  // Caller releases via `free_image_bgra8_w32`. Returns nullptr on failure.
  inline uint8_t* load_image_bgra8_w32_memory(const uint8_t* data, size_t len,
                                              uint32_t* width_out,
                                              uint32_t* height_out,
                                              const WICPixelFormatGUID& pixel_format
                                                = GUID_WICPixelFormat32bppPBGRA)
  {
    if (!data || len == 0 || len > 0xFFFFFFFFull) return nullptr;
    IWICImagingFactory* wic = wic_factory_w32();
    if (!wic) return nullptr;

    IStream* stream = SHCreateMemStream(static_cast<const BYTE*>(data),
                                        static_cast<UINT>(len));
    if (!stream) return nullptr;

    IWICBitmapDecoder* decoder = nullptr;
    uint8_t*           result  = nullptr;
    if (SUCCEEDED(wic->CreateDecoderFromStream(
            stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder))) {
      result = wic_decode_frame_w32(wic, decoder, pixel_format,
                                    width_out, height_out);
      decoder->Release();
    }
    stream->Release();
    return result;
  }

  // Decode `path` into a heap-allocated BGRA8 buffer.
  // `pixel_format` selects the WIC output format:
  //   - GUID_WICPixelFormat32bppPBGRA (default) - premultiplied alpha,
  //     correct for D2D / cg bitmap upload (the asset manager).
  //   - GUID_WICPixelFormat32bppBGRA            - straight alpha, needed
  //     for the BITMAPV5HEADER + CreateIconIndirect path in icon_win32.h
  //     so GDI doesn't re-multiply premultiplied edges.
  // Caller releases via `free_image_bgra8_w32` (delete[]).
  // Returns nullptr on failure.
  inline uint8_t* load_image_bgra8_w32(const char* path,
                                        uint32_t* width_out,
                                        uint32_t* height_out,
                                        const WICPixelFormatGUID& pixel_format
                                          = GUID_WICPixelFormat32bppPBGRA)
  {
    if (!path || !width_out || !height_out) return nullptr;
    IWICImagingFactory* wic = wic_factory_w32();
    if (!wic) return nullptr;

    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (n <= 1) return nullptr;
    std::wstring wpath(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path, -1, &wpath[0], n);

    // Embedded "PNG" resource: name = the caller-supplied path wrapped
    // in literal quotes (the rc.exe quirk above). Resource memory has
    // process lifetime, so decoding straight out of it is safe.
    std::wstring res_name = L"\"" + wpath + L"\"";
    if (HMODULE hMod = GetModuleHandleW(nullptr)) {
      if (HRSRC hRes = FindResourceW(hMod, res_name.c_str(), L"PNG")) {
        DWORD   rsize = SizeofResource(hMod, hRes);
        HGLOBAL hGlob = LoadResource(hMod, hRes);
        if (hGlob && rsize > 0) {
          if (void* rdata = LockResource(hGlob)) {
            if (uint8_t* res = load_image_bgra8_w32_memory(
                    static_cast<const uint8_t*>(rdata), rsize,
                    width_out, height_out, pixel_format))
              return res;
            // Fall through to the file branch on a bad resource.
          }
        }
      }
    }

    IWICBitmapDecoder* decoder = nullptr;
    if (FAILED(wic->CreateDecoderFromFilename(
            wpath.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &decoder)))
      return nullptr;

    uint8_t* result = wic_decode_frame_w32(wic, decoder, pixel_format,
                                          width_out, height_out);
    decoder->Release();
    return result;
  }

  inline void free_image_bgra8_w32(uint8_t* pixels) { delete[] pixels; }

} // namespace neui_detail

#endif // _WIN32
