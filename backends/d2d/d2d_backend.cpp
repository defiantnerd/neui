#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d2d1effects_2.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwrite.h>
#include <dwrite_3.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "d2d_backend.h"
#include "../shared/backend_util.h"

#pragma comment(lib, "d2d1")
#pragma comment(lib, "d3d11")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "dwrite")
#pragma comment(lib, "dxguid")  // provides CLSID_D2D1Tint and other effect CLSIDs

namespace neui_d2d_backend
{
  // Active font state. Pushed/popped via push_font/pop_font; the top of
  // the stack is combined with each draw_text/measure_text call's
  // font_size to look up an IDWriteTextFormat in the cache. Empty
  // family => "Segoe UI" (host default); weight 0 => DWRITE_FONT_WEIGHT_NORMAL.
  struct FontState
  {
    std::wstring family;   // empty = host default (Segoe UI)
    int          weight = 0;  // 0 = DWRITE_FONT_WEIGHT_NORMAL (400)
  };

  // Per-window or per-surface render context. The active render target
  // (`target`) is an ID2D1DeviceContext. For HWND contexts it draws into
  // `back_buffer`, a D2D bitmap aliased to a DXGI swap-chain back buffer
  // that we Present1 at end_frame. For NEUI_ASSET_KIND_SURFACE contexts
  // it draws into a GPU target bitmap; read_pixels_bgra copies that into
  // a staging bitmap (CANNOT_DRAW | CPU_READ) and Maps it to memcpy out.
  //
  // The HWND-vs-offscreen discriminator is `swap_chain != nullptr` (HWND
  // path) vs `surface_w_px != 0` (offscreen path).
  struct D2DContext
  {
    // Active render target + brush. All draw paths use ID2D1RenderTarget's
    // ABI subset that ID2D1DeviceContext inherits; effects use the device
    // context directly.
    ID2D1DeviceContext*    target      = nullptr;
    ID2D1SolidColorBrush*  brush       = nullptr;
    uint32_t               dpi         = 96;

    // HWND-specific. The DXGI swap chain owns the back-buffer texture;
    // `back_buffer` is the D2D-side alias bound as the device context's
    // current target. hwnd + size are stashed at create time and kept
    // in sync by d2d_resize / d2d_update_dpi. If a draw operation
    // returns D2DERR_RECREATE_TARGET (or DXGI_ERROR_DEVICE_REMOVED on
    // Present1), the device is gone but the D2DContext pointer stays
    // valid: the next begin_frame rebuilds the device + swap chain in
    // place, bumps `generation`, and lets cached per-ctx bitmap handles
    // discover their staleness via get_context_generation.
    IDXGISwapChain1*       swap_chain  = nullptr;
    ID2D1Bitmap1*          back_buffer = nullptr;
    HWND                   hwnd        = nullptr;
    uint32_t               width       = 0;
    uint32_t               height      = 0;
    uint32_t               generation  = 1;

    // Off-screen-specific. With D2D 1.1, a target bitmap created via
    // CreateBitmap[FromWicBitmap] with D2D1_BITMAP_OPTIONS_TARGET is a
    // GPU texture - it cannot be CPU-mapped directly. read_pixels_bgra
    // copies the target's pixels into a staging bitmap created with
    // CANNOT_DRAW | CPU_READ, then Maps the staging bitmap to memcpy
    // the bytes out. `back_buffer` (above) holds the render target;
    // `staging_bitmap` is lazy-allocated on the first readback and
    // reused for subsequent reads of the same surface size. Both
    // fields stay null on HWND ctxs.
    ID2D1Bitmap1*          staging_bitmap = nullptr;
    uint32_t               surface_w_px   = 0;
    uint32_t               surface_h_px   = 0;

    // Path API state. The path is constructed via move_to/line_to/arc;
    // figure_open tracks whether BeginFigure has been called without a
    // matching EndFigure. fill_path / stroke_path close the figure and the
    // sink, then draw. The path stays valid for additional fill/stroke
    // calls until the next begin_path.
    ID2D1PathGeometry*     path        = nullptr;
    ID2D1GeometrySink*     sink        = nullptr;
    bool                   figure_open = false;
    bool                   sink_closed = false;
    D2D1_POINT_2F          cursor_pt   = { 0.0f, 0.0f };

    // Transform stack. `current` is the active D2D world transform, applied
    // via ID2D1RenderTarget::SetTransform whenever it changes. push pushes
    // a copy of `current` onto the stack; pop restores it. Reset to
    // identity on every begin_frame.
    D2D1::Matrix3x2F            current{ D2D1::Matrix3x2F::Identity() };
    std::vector<D2D1::Matrix3x2F> transform_stack;

    // Alpha stack. Stores cumulative opacity factors; the back() is the
    // effective alpha multiplier applied to every draw call. Empty stack
    // means effective 1.0. Reset on every begin_frame.
    std::vector<float>          alpha_stack;

    // Font stack. back() is the active (family, weight) pair feeding
    // draw_text / measure_text; empty stack means "host default at the
    // call's font_size". Reset on every begin_frame so a missing pop in
    // one frame can't bleed across.
    std::vector<FontState>      font_stack;

    // Tint effect, lazily created the first time a tinted draw_bitmap is
    // issued against this ctx. Released alongside `target` on device-loss
    // / destroy. Reused across draws within a frame.
    ID2D1Effect*                tint_effect = nullptr;
  };

  // Process-wide D2D / DWrite / DXGI factories - created once, never
  // destroyed (lives for process lifetime). g_factory is the 1.1 factory
  // (ID2D1Factory1), required to reach CreateDevice + the effects framework.
  static ID2D1Factory1*      g_factory        = nullptr;
  static IDWriteFactory*     g_dwrite_factory = nullptr;
  static IDXGIFactory2*      g_dxgi_factory   = nullptr;

  // Process-wide D3D11 + D2D device. Both HWND ctxs and offscreen ctxs
  // share these - the D2D device is the source of every ID2D1DeviceContext
  // we hand back, and the DXGI device under it is used to make swap chains
  // for HWND ctxs. We rebuild them on device-loss and bump every ctx's
  // generation in begin_frame so cached resources re-upload.
  static ID3D11Device*       g_d3d_device     = nullptr;
  static IDXGIDevice*        g_dxgi_device    = nullptr;
  static ID2D1Device*        g_d2d_device     = nullptr;

  // Text format cache, keyed by (family, weight, size). Size is quantised
  // to 0.1 logical pixels so floating-point chatter (12.0 vs 12.00001)
  // doesn't churn cache entries. Entries are never evicted; a typical
  // app uses a handful of (family, weight, size) tuples.
  struct TextFormatKey
  {
    std::wstring family;
    int          weight = 0;     // DWRITE_FONT_WEIGHT_* (100..950)
    uint32_t     size_q10 = 0;   // round(font_size * 10)

    bool operator==(const TextFormatKey& other) const {
      return weight == other.weight
          && size_q10 == other.size_q10
          && family == other.family;
    }
  };

  struct TextFormatKeyHash
  {
    size_t operator()(const TextFormatKey& k) const noexcept {
      // Cheap composition; family hash dominates if families differ.
      size_t h = std::hash<std::wstring>{}(k.family);
      h ^= std::hash<int>{}(k.weight)        + 0x9e3779b9 + (h << 6) + (h >> 2);
      h ^= std::hash<uint32_t>{}(k.size_q10) + 0x9e3779b9 + (h << 6) + (h >> 2);
      return h;
    }
  };

  static std::unordered_map<TextFormatKey, IDWriteTextFormat*, TextFormatKeyHash>
    g_text_format_cache;

  // Tear down the process-wide D3D / DXGI / D2D devices. Called when
  // begin_frame discovers the device is gone, so the next ctx rebuild
  // creates a fresh device chain. The DWrite factory is not device-bound
  // and stays alive across rebuilds.
  static void release_device_chain()
  {
    if (g_d2d_device)  { g_d2d_device->Release();  g_d2d_device  = nullptr; }
    if (g_dxgi_device) { g_dxgi_device->Release(); g_dxgi_device = nullptr; }
    if (g_d3d_device)  { g_d3d_device->Release();  g_d3d_device  = nullptr; }
  }

  // Build the D3D11 + DXGI + D2D device chain. Idempotent: if everything
  // is already built, returns true immediately. Called from
  // d2d_create_context and from d2d_begin_frame's device-loss recovery path.
  static bool ensure_device_chain()
  {
    if (g_d2d_device && g_d3d_device && g_dxgi_device) return true;
    release_device_chain();

    // D3D11 device for the GPU surface. BGRA_SUPPORT is required for D2D
    // interop. We pick the default hardware adapter; if hardware is
    // unavailable (Remote Desktop with software fallback policy), fall
    // back to the WARP rasteriser so we still get a working device.
    D3D_FEATURE_LEVEL fl;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr,
                                    D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                    flags, nullptr, 0,
                                    D3D11_SDK_VERSION,
                                    &g_d3d_device, &fl, nullptr);
    if (FAILED(hr)) {
      hr = D3D11CreateDevice(nullptr,
                              D3D_DRIVER_TYPE_WARP, nullptr,
                              flags, nullptr, 0,
                              D3D11_SDK_VERSION,
                              &g_d3d_device, &fl, nullptr);
    }
    if (FAILED(hr) || !g_d3d_device) { release_device_chain(); return false; }

    hr = g_d3d_device->QueryInterface(IID_PPV_ARGS(&g_dxgi_device));
    if (FAILED(hr)) { release_device_chain(); return false; }

    hr = g_factory->CreateDevice(g_dxgi_device, &g_d2d_device);
    if (FAILED(hr)) { release_device_chain(); return false; }
    return true;
  }

  static bool ensure_factory()
  {
    if (g_factory) return true;
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                    __uuidof(ID2D1Factory1),
                                    reinterpret_cast<void**>(&g_factory));
    if (FAILED(hr)) return false;
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                             __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(&g_dwrite_factory));
    if (FAILED(hr)) return false;
    hr = CreateDXGIFactory1(IID_PPV_ARGS(&g_dxgi_factory));
    return SUCCEEDED(hr);
  }

  // --- Client-registered fonts (NEUI_ASSET_KIND_FONT) -----------------------
  //
  // Registered fonts are merged into a custom IDWriteFontCollection1 that
  // get_text_format consults instead of the system collection. The merged
  // set ALSO includes the system font set, so passing it to CreateTextFormat
  // resolves both custom and system families - unknown families still fall
  // back to the default in get_text_format's "Segoe UI" branch.
  static IDWriteFactory3*               g_dwrite3          = nullptr;
  static IDWriteFactory5*               g_dwrite5          = nullptr; // in-memory loader (Win10 1709+)
  static IDWriteInMemoryFontFileLoader* g_inmem_loader     = nullptr;
  static IDWriteFontCollection1*        g_custom_collection = nullptr;

  struct CustomFontEntry { uint64_t token = 0; IDWriteFontFile* file = nullptr; };
  static std::vector<CustomFontEntry>   g_custom_fonts;
  static uint64_t                       g_next_font_token  = 1;

  // QI the shared DWrite factory up to the v3 (font-set / collection-from-set)
  // and optional v5 (in-memory loader) interfaces. v3 is required for any
  // font registration; v5 only for the in-memory form. Both are Win10; a
  // failed QI returns false so create_font degrades to asset_none.
  static bool ensure_dwrite3()
  {
    if (!g_dwrite_factory) return false;
    if (!g_dwrite3) g_dwrite_factory->QueryInterface(IID_PPV_ARGS(&g_dwrite3));
    if (!g_dwrite3) return false;
    if (!g_dwrite5) g_dwrite_factory->QueryInterface(IID_PPV_ARGS(&g_dwrite5));
    return true;
  }

  static std::string wide_to_utf8(const std::wstring& w)
  {
    if (w.empty()) return {};
    int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0,
                                    nullptr, nullptr);
    if (need <= 1) return {};
    std::string s(static_cast<size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), need,
                         nullptr, nullptr);
    return s;
  }

  static std::wstring utf8_to_wide(const char* s)
  {
    std::wstring w;
    if (!s || !*s) return w;
    int need = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (need > 1) {
      w.resize(static_cast<size_t>(need - 1));
      MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), need);
    }
    return w;
  }

  // Read the WIN32 family name of the first face in a font file.
  static bool read_family_from_file(IDWriteFontFile* file, std::wstring& out)
  {
    if (!g_dwrite3 || !file) return false;
    BOOL                  supported = FALSE;
    DWRITE_FONT_FILE_TYPE ftype     = DWRITE_FONT_FILE_TYPE_UNKNOWN;
    DWRITE_FONT_FACE_TYPE fctype    = DWRITE_FONT_FACE_TYPE_UNKNOWN;
    UINT32                num_faces = 0;
    if (FAILED(file->Analyze(&supported, &ftype, &fctype, &num_faces))
     || !supported)
      return false;

    IDWriteFontSetBuilder* sb = nullptr;
    if (FAILED(g_dwrite3->CreateFontSetBuilder(&sb)) || !sb) return false;
    // AddFontFile lives on the v1 builder (Win10 1703+); fail gracefully if
    // unavailable so create_font degrades to asset_none.
    IDWriteFontSetBuilder1* sb1 = nullptr;
    if (FAILED(sb->QueryInterface(IID_PPV_ARGS(&sb1))) || !sb1) {
      sb->Release();
      return false;
    }
    HRESULT hr = sb1->AddFontFile(file);
    sb1->Release();
    if (FAILED(hr)) { sb->Release(); return false; }
    IDWriteFontSet* set = nullptr;
    hr = sb->CreateFontSet(&set);
    sb->Release();
    if (FAILED(hr) || !set) return false;

    bool ok = false;
    if (set->GetFontCount() > 0) {
      BOOL exists = FALSE;
      IDWriteLocalizedStrings* names = nullptr;
      if (SUCCEEDED(set->GetPropertyValues(
            0, DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME, &exists, &names))
       && exists && names && names->GetCount() > 0) {
        UINT32 len = 0;
        if (SUCCEEDED(names->GetStringLength(0, &len)) && len > 0) {
          std::vector<wchar_t> buf(len + 1u, 0);
          if (SUCCEEDED(names->GetString(0, buf.data(), len + 1u))) {
            out.assign(buf.data());
            ok = !out.empty();
          }
        }
      }
      if (names) names->Release();
    }
    set->Release();
    return ok;
  }

  // Drop every cached IDWriteTextFormat so the next get_text_format rebuilds
  // against the current g_custom_collection. Without this, a (family, weight,
  // size) drawn *before* its font was registered would keep resolving to the
  // cached system-only fallback even after registration (mirrors the CG
  // backend's flush_font_cache). Called from rebuild_custom_collection, the
  // single point both register and unregister funnel through.
  static void flush_text_format_cache()
  {
    for (auto& kv : g_text_format_cache)
      if (kv.second) kv.second->Release();
    g_text_format_cache.clear();
  }

  // Rebuild g_custom_collection = system font set + every registered file.
  static void rebuild_custom_collection()
  {
    flush_text_format_cache();
    if (g_custom_collection) { g_custom_collection->Release(); g_custom_collection = nullptr; }
    if (!g_dwrite3 || g_custom_fonts.empty()) return;

    IDWriteFontSetBuilder* sb = nullptr;
    if (FAILED(g_dwrite3->CreateFontSetBuilder(&sb)) || !sb) return;

    // AddFontFile / AddFontSet both live on the v1 builder (Win10 1703+).
    // Without it we can't add the custom files at all, so bail (the previous
    // collection was already released above; null = system-only resolution).
    IDWriteFontSetBuilder1* sb1 = nullptr;
    if (FAILED(sb->QueryInterface(IID_PPV_ARGS(&sb1))) || !sb1) {
      sb->Release();
      return;
    }

    // Fold in the system fonts so the merged collection resolves system
    // families too (CreateTextFormat takes a single collection).
    IDWriteFontSet* sysset = nullptr;
    if (SUCCEEDED(g_dwrite3->GetSystemFontSet(&sysset)) && sysset) {
      sb1->AddFontSet(sysset);
      sysset->Release();
    }
    for (auto& e : g_custom_fonts)
      if (e.file) sb1->AddFontFile(e.file);

    IDWriteFontSet* set = nullptr;
    if (SUCCEEDED(sb->CreateFontSet(&set)) && set) {
      g_dwrite3->CreateFontCollectionFromFontSet(set, &g_custom_collection);
      set->Release();
    }
    sb1->Release();
    sb->Release();
  }

  // Take ownership of `file` (AddRef'd into the registry), read its family
  // name, and rebuild the merged collection. Common tail of the memory /
  // file register entry points.
  static bool d2d_register_common(IDWriteFontFile* file,
                                   char* out_family, uint32_t cap,
                                   uint64_t* out_token)
  {
    std::wstring fam;
    if (!read_family_from_file(file, fam)) return false;

    CustomFontEntry e;
    e.token = g_next_font_token++;
    e.file  = file;
    file->AddRef();
    g_custom_fonts.push_back(e);
    rebuild_custom_collection();

    if (out_family && cap > 0) {
      std::string u = wide_to_utf8(fam);
      uint32_t n = static_cast<uint32_t>(u.size());
      if (n > cap - 1) n = cap - 1;
      if (n) std::memcpy(out_family, u.data(), n);
      out_family[n] = '\0';
    }
    if (out_token) *out_token = e.token;
    return true;
  }

  static bool d2d_register_font(const uint8_t* data, uint32_t len,
                                 char* out_family, uint32_t cap,
                                 uint64_t* out_token)
  {
    if (out_family && cap) out_family[0] = '\0';
    if (out_token) *out_token = 0;
    if (!data || len == 0) return false;
    if (!ensure_factory() || !ensure_dwrite3() || !g_dwrite5) return false;

    if (!g_inmem_loader) {
      if (FAILED(g_dwrite5->CreateInMemoryFontFileLoader(&g_inmem_loader))
       || !g_inmem_loader)
        return false;
      g_dwrite5->RegisterFontFileLoader(g_inmem_loader);
    }

    IDWriteFontFile* file = nullptr;
    // ownerObject == null: the loader copies the data, so registration does
    // not depend on the caller's buffer surviving - but the asset store keeps
    // it alive regardless, matching the documented cross-backend contract.
    HRESULT hr = g_inmem_loader->CreateInMemoryFontFileReference(
      g_dwrite_factory, data, len, nullptr, &file);
    if (FAILED(hr) || !file) return false;

    bool ok = d2d_register_common(file, out_family, cap, out_token);
    file->Release();
    return ok;
  }

  static bool d2d_register_font_file(const char* path,
                                      char* out_family, uint32_t cap,
                                      uint64_t* out_token)
  {
    if (out_family && cap) out_family[0] = '\0';
    if (out_token) *out_token = 0;
    if (!path || !*path) return false;
    if (!ensure_factory() || !ensure_dwrite3()) return false;

    std::wstring wpath = utf8_to_wide(path);
    if (wpath.empty()) return false;
    IDWriteFontFile* file = nullptr;
    if (FAILED(g_dwrite_factory->CreateFontFileReference(wpath.c_str(),
                                                          nullptr, &file))
     || !file)
      return false;

    bool ok = d2d_register_common(file, out_family, cap, out_token);
    file->Release();
    return ok;
  }

  static void d2d_unregister_font(uint64_t token)
  {
    if (!token) return;
    for (auto it = g_custom_fonts.begin(); it != g_custom_fonts.end(); ++it) {
      if (it->token == token) {
        if (it->file) it->file->Release();
        g_custom_fonts.erase(it);
        rebuild_custom_collection();
        return;
      }
    }
  }

  // Snap a CSS-style weight (100..900) to the nearest DWRITE_FONT_WEIGHT
  // bucket. 0 / out-of-range => Normal (400).
  static DWRITE_FONT_WEIGHT normalise_weight(int weight)
  {
    if (weight <= 0)   return DWRITE_FONT_WEIGHT_NORMAL;
    if (weight < 150)  return DWRITE_FONT_WEIGHT_THIN;        // 100
    if (weight < 250)  return DWRITE_FONT_WEIGHT_EXTRA_LIGHT; // 200
    if (weight < 350)  return DWRITE_FONT_WEIGHT_LIGHT;       // 300
    if (weight < 450)  return DWRITE_FONT_WEIGHT_NORMAL;      // 400
    if (weight < 550)  return DWRITE_FONT_WEIGHT_MEDIUM;      // 500
    if (weight < 650)  return DWRITE_FONT_WEIGHT_SEMI_BOLD;   // 600
    if (weight < 750)  return DWRITE_FONT_WEIGHT_BOLD;        // 700
    if (weight < 850)  return DWRITE_FONT_WEIGHT_EXTRA_BOLD;  // 800
    return DWRITE_FONT_WEIGHT_BLACK;                          // 900+
  }

  // Resolve the active font for this context (top of font_stack, or
  // host default when empty) into a cached IDWriteTextFormat at the
  // call's logical font size. Returns nullptr on failure.
  static IDWriteTextFormat* get_text_format(D2DContext* ctx, float font_size)
  {
    if (!g_dwrite_factory) return nullptr;

    const FontState* fs = (ctx && !ctx->font_stack.empty())
      ? &ctx->font_stack.back()
      : nullptr;

    TextFormatKey key;
    if (fs && !fs->family.empty()) key.family = fs->family;
    else                            key.family = L"Segoe UI";
    key.weight   = static_cast<int>(normalise_weight(fs ? fs->weight : 0));
    key.size_q10 = neui_detail::font_size_q10(font_size);

    auto it = g_text_format_cache.find(key);
    if (it != g_text_format_cache.end()) return it->second;

    IDWriteTextFormat* fmt = nullptr;
    // g_custom_collection (when present) merges the system fonts with every
    // client-registered family, so it resolves both; null = system only.
    HRESULT hr = g_dwrite_factory->CreateTextFormat(
      key.family.c_str(),
      g_custom_collection,
      static_cast<DWRITE_FONT_WEIGHT>(key.weight),
      DWRITE_FONT_STYLE_NORMAL,
      DWRITE_FONT_STRETCH_NORMAL,
      font_size,
      L"",
      &fmt
    );
    if (FAILED(hr)) return nullptr;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g_text_format_cache[key] = fmt;
    return fmt;
  }

  static D2D1_COLOR_F argb_to_color(uint32_t argb, float alpha_mul = 1.0f)
  {
    float c[4];
    neui_detail::argb_unpack(argb, c, alpha_mul);
    return D2D1::ColorF(c[0], c[1], c[2], c[3]);
  }

  static inline float current_alpha(const D2DContext* ctx)
  {
    return neui_detail::alpha_stack_current(ctx->alpha_stack);
  }

  // ---------------------------------------------------------------------------

  // Release the per-ctx D2D objects that depend on the live device chain
  // (target, brush, back-buffer alias, swap chain, tint effect). Called
  // from destroy_context and from begin_frame's device-loss rebuild path.
  // hwnd / size / dpi / generation are intentionally preserved so begin_frame
  // can rebuild against the same window.
  static void release_target_objects(D2DContext* ctx)
  {
    if (!ctx) return;
    if (ctx->tint_effect) { ctx->tint_effect->Release(); ctx->tint_effect = nullptr; }
    if (ctx->brush)       { ctx->brush->Release();       ctx->brush       = nullptr; }
    if (ctx->back_buffer) { ctx->back_buffer->Release(); ctx->back_buffer = nullptr; }
    if (ctx->target)      { ctx->target->Release();      ctx->target      = nullptr; }
    if (ctx->swap_chain)  { ctx->swap_chain->Release();  ctx->swap_chain  = nullptr; }
  }

  // Build target + swap chain + back-buffer bitmap + brush for an HWND ctx
  // using the stored hwnd / size / dpi. Used by d2d_create_context and the
  // device-loss recovery path in d2d_begin_frame; populates ctx->target /
  // ctx->swap_chain / ctx->back_buffer / ctx->brush on success and leaves
  // them null on failure. Does not touch ctx->generation - callers bump
  // that when they want the recreation to be visible to bitmap caches.
  static bool d2d_build_hwnd_target(D2DContext* ctx)
  {
    if (!ctx || !ctx->hwnd || ctx->width == 0 || ctx->height == 0) return false;
    if (!ensure_factory()) return false;
    if (!ensure_device_chain()) return false;

    // Per-window device context. Bound to the shared D2D device.
    ID2D1DeviceContext* dc = nullptr;
    HRESULT hr = g_d2d_device->CreateDeviceContext(
      D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
    if (FAILED(hr) || !dc) return false;
    dc->SetDpi(static_cast<float>(ctx->dpi), static_cast<float>(ctx->dpi));

    // DXGI swap chain bound to the HWND. Width/Height = 0 lets DXGI track
    // the window's current client size; we still cache the explicit size
    // for resize bookkeeping.
    DXGI_SWAP_CHAIN_DESC1 sc = {};
    sc.Width            = ctx->width;
    sc.Height           = ctx->height;
    sc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    sc.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount      = 2;
    sc.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sc.SampleDesc.Count = 1;
    sc.Scaling          = DXGI_SCALING_NONE;
    sc.AlphaMode        = DXGI_ALPHA_MODE_IGNORE;
    IDXGISwapChain1* swap = nullptr;
    hr = g_dxgi_factory->CreateSwapChainForHwnd(g_d3d_device, ctx->hwnd,
                                                 &sc, nullptr, nullptr, &swap);
    if (FAILED(hr) || !swap) {
      // Some DXGI configurations (older drivers under Remote Desktop) don't
      // support flip-sequential + SCALING_NONE - retry with stretch scaling.
      sc.Scaling = DXGI_SCALING_STRETCH;
      hr = g_dxgi_factory->CreateSwapChainForHwnd(g_d3d_device, ctx->hwnd,
                                                   &sc, nullptr, nullptr, &swap);
    }
    if (FAILED(hr) || !swap) { dc->Release(); return false; }

    // D2D bitmap aliased to the swap chain's back buffer surface.
    IDXGISurface* back = nullptr;
    hr = swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (FAILED(hr) || !back) { swap->Release(); dc->Release(); return false; }

    D2D1_BITMAP_PROPERTIES1 bp = {};
    bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    bp.pixelFormat   = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                          D2D1_ALPHA_MODE_IGNORE);
    bp.dpiX = static_cast<float>(ctx->dpi);
    bp.dpiY = static_cast<float>(ctx->dpi);
    ID2D1Bitmap1* bb = nullptr;
    hr = dc->CreateBitmapFromDxgiSurface(back, &bp, &bb);
    back->Release();
    if (FAILED(hr) || !bb) { swap->Release(); dc->Release(); return false; }

    dc->SetTarget(bb);

    ID2D1SolidColorBrush* brush = nullptr;
    hr = dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush);
    if (FAILED(hr)) {
      bb->Release(); swap->Release(); dc->Release();
      return false;
    }

    ctx->target      = dc;
    ctx->swap_chain  = swap;
    ctx->back_buffer = bb;
    ctx->brush       = brush;
    return true;
  }

  static neui_render_ctx_t d2d_create_context(void* native_handle,
                                               uint32_t width, uint32_t height)
  {
    if (!ensure_factory()) return nullptr;

    HWND hwnd = reinterpret_cast<HWND>(native_handle);
    UINT dpi  = GetDpiForWindow(hwnd);

    auto* ctx  = new D2DContext();
    ctx->hwnd   = hwnd;
    ctx->width  = width;
    ctx->height = height;
    ctx->dpi    = dpi;
    if (!d2d_build_hwnd_target(ctx)) { delete ctx; return nullptr; }
    return ctx;
  }

  static void d2d_destroy_context(neui_render_ctx_t raw)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx) return;
    if (ctx->sink)   { ctx->sink->Release();   ctx->sink   = nullptr; }
    if (ctx->path)   { ctx->path->Release();   ctx->path   = nullptr; }
    if (ctx->staging_bitmap) {
      ctx->staging_bitmap->Release();
      ctx->staging_bitmap = nullptr;
    }
    release_target_objects(ctx);
    delete ctx;
  }

  static void d2d_resize(neui_render_ctx_t raw, uint32_t width, uint32_t height)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->swap_chain) return;  // resize is HWND-only
    if (width == 0 || height == 0) return;
    ctx->width  = width;
    ctx->height = height;

    // Drop the D2D back-buffer alias before resizing the swap chain (DXGI
    // requires every reference to the back-buffer surface to be released).
    if (ctx->target) ctx->target->SetTarget(nullptr);
    if (ctx->back_buffer) { ctx->back_buffer->Release(); ctx->back_buffer = nullptr; }

    HRESULT hr = ctx->swap_chain->ResizeBuffers(0, width, height,
                                                  DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
      // Treat resize failure as device-loss: drop the target objects so the
      // next begin_frame rebuilds and bumps generation.
      release_target_objects(ctx);
      return;
    }

    // Re-acquire the back-buffer alias and reattach to the device context.
    IDXGISurface* back = nullptr;
    hr = ctx->swap_chain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (FAILED(hr) || !back) { release_target_objects(ctx); return; }

    D2D1_BITMAP_PROPERTIES1 bp = {};
    bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    bp.pixelFormat   = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                          D2D1_ALPHA_MODE_IGNORE);
    bp.dpiX = static_cast<float>(ctx->dpi);
    bp.dpiY = static_cast<float>(ctx->dpi);
    ID2D1Bitmap1* bb = nullptr;
    hr = ctx->target->CreateBitmapFromDxgiSurface(back, &bp, &bb);
    back->Release();
    if (FAILED(hr) || !bb) { release_target_objects(ctx); return; }
    ctx->target->SetTarget(bb);
    ctx->back_buffer = bb;
  }

  static void d2d_begin_frame(neui_render_ctx_t raw, uint32_t clear_argb)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx) return;
    if (!ctx->target && ctx->hwnd) {
      // Previous frame lost the device on an HWND ctx (D2DERR_RECREATE_TARGET
      // or DXGI_ERROR_DEVICE_REMOVED). Tear the process-wide device chain so
      // ensure_device_chain rebuilds from scratch, then re-create our ctx's
      // target objects. Bump generation so cached per-ctx bitmaps know to
      // re-upload. If rebuild fails (driver still in a bad state) silently
      // skip this frame; the next begin_frame retries. Off-screen surface
      // ctxs do not take this path - their target stays alive for life.
      release_device_chain();
      if (!d2d_build_hwnd_target(ctx)) return;
      ctx->generation++;
    }
    if (!ctx->target) return;
    ctx->target->BeginDraw();
    // Reset transform stack to identity each frame so a missing pop in
    // a previous frame can't bleed across frame boundaries.
    ctx->current = D2D1::Matrix3x2F::Identity();
    ctx->transform_stack.clear();
    ctx->alpha_stack.clear();
    ctx->font_stack.clear();
    ctx->target->SetTransform(ctx->current);
    ctx->target->Clear(argb_to_color(clear_argb));
  }

  static void d2d_end_frame(neui_render_ctx_t raw)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target) return;
    HRESULT hr = ctx->target->EndDraw();
    bool lost = (hr == D2DERR_RECREATE_TARGET);

    // For HWND ctxs, Present1 ships the frame to the desktop compositor.
    // DXGI_ERROR_DEVICE_REMOVED / DXGI_ERROR_DEVICE_RESET are the DXGI-side
    // signal that the device chain is gone; treat them like D2DERR_RECREATE_TARGET.
    if (SUCCEEDED(hr) && ctx->swap_chain) {
      DXGI_PRESENT_PARAMETERS pp = {};
      hr = ctx->swap_chain->Present1(1, 0, &pp);
      if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        lost = true;
    }

    if (lost && ctx->swap_chain) {
      // Device lost on an HWND ctx. Drop the live target objects and the
      // shared device chain so begin_frame rebuilds. ID2D1Bitmap handles
      // created against this target are now dangling for draw purposes but
      // still safe to Release(); the asset manager (one per session) owns
      // those caches and discovers their staleness via the next
      // get_context_generation mismatch.
      release_target_objects(ctx);
      release_device_chain();
    }
  }

  static uint32_t d2d_get_context_generation(neui_render_ctx_t raw)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    return ctx ? ctx->generation : 0u;
  }

  static void d2d_fill_rect(neui_render_ctx_t raw,
                             float x, float y, float w, float h,
                             uint32_t argb)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target || !ctx->brush) return;
    ctx->brush->SetColor(argb_to_color(argb, current_alpha(ctx)));
    // Coordinates are logical (96 DPI); D2D target already has DPI set, so pass as-is.
    ctx->target->FillRectangle(D2D1::RectF(x, y, x + w, y + h), ctx->brush);
  }

  static void d2d_draw_rect(neui_render_ctx_t raw,
                             float x, float y, float w, float h,
                             float stroke_width,
                             uint32_t argb)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target || !ctx->brush) return;
    ctx->brush->SetColor(argb_to_color(argb, current_alpha(ctx)));
    ctx->target->DrawRectangle(D2D1::RectF(x, y, x + w, y + h), ctx->brush, stroke_width);
  }

  static float d2d_get_scale_factor(neui_render_ctx_t raw)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx) return 1.0f;
    return static_cast<float>(ctx->dpi) / 96.0f;
  }

  static void d2d_update_dpi(neui_render_ctx_t raw, uint32_t dpi)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx) return;
    ctx->dpi = dpi;
    // Mirror the new DPI onto the live target if present; if we're in the
    // lost-device window the rebuild in d2d_begin_frame will use the
    // stashed value.
    if (ctx->target)
      ctx->target->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
  }

  static void d2d_draw_text(neui_render_ctx_t raw,
                             float x, float y, float w, float h,
                             const char* text,
                             float font_size,
                             uint32_t argb)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target || !ctx->brush || !text || !*text) return;

    IDWriteTextFormat* fmt = get_text_format(ctx, font_size);
    if (!fmt) return;

    // Convert UTF-8 to UTF-16.
    int needed = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (needed <= 1) return;  // empty or error
    wchar_t  stack_buf[256];
    wchar_t* wbuf = (needed <= 256) ? stack_buf : new wchar_t[needed];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wbuf, needed);
    int len = needed - 1;  // exclude null terminator

    ctx->brush->SetColor(argb_to_color(argb, current_alpha(ctx)));
    D2D1_RECT_F rect = D2D1::RectF(x, y, x + w, y + h);
    ctx->target->DrawText(
      wbuf, static_cast<UINT32>(len),
      fmt,
      rect,
      ctx->brush,
      D2D1_DRAW_TEXT_OPTIONS_CLIP
    );

    if (wbuf != stack_buf) delete[] wbuf;
  }

  static float d2d_measure_text(neui_render_ctx_t raw,
                                 const char* text, int text_len,
                                 float font_size)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!g_dwrite_factory || !text || !*text) return 0.0f;
    IDWriteTextFormat* fmt = get_text_format(ctx, font_size);
    if (!fmt) return 0.0f;

    // Convert UTF-8 -> UTF-16, honouring text_len byte limit.
    int byte_len = (text_len < 0) ? -1 : text_len;
    int needed = MultiByteToWideChar(CP_UTF8, 0, text, byte_len, nullptr, 0);
    if (needed <= 0) return 0.0f;
    // MultiByteToWideChar with byte_len >= 0 does NOT append a null terminator.
    wchar_t  stack_buf[256];
    wchar_t* wbuf = (needed <= 256) ? stack_buf : new wchar_t[needed];
    MultiByteToWideChar(CP_UTF8, 0, text, byte_len, wbuf, needed);

    IDWriteTextLayout* layout = nullptr;
    HRESULT hr = g_dwrite_factory->CreateTextLayout(
      wbuf, static_cast<UINT32>(needed),
      fmt,
      100000.0f, 100000.0f,   // very large max - we want unconstrained width
      &layout
    );

    if (wbuf != stack_buf) delete[] wbuf;
    if (FAILED(hr) || !layout) return 0.0f;

    DWRITE_TEXT_METRICS metrics = {};
    layout->GetMetrics(&metrics);
    layout->Release();
    return metrics.widthIncludingTrailingWhitespace;
  }

  static void* d2d_create_bitmap(neui_render_ctx_t raw,
                                  uint32_t width_px, uint32_t height_px,
                                  const uint8_t* bgra_pixels,
                                  float scale)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target || !bgra_pixels || width_px == 0 || height_px == 0)
      return nullptr;

    // Set the bitmap's DPI to scale*96 so that GetSize() returns logical dimensions
    // (physical_px / scale) and src coordinates in draw calls use logical pixels.
    float bmp_dpi = scale * 96.0f;
    D2D1_BITMAP_PROPERTIES props = {};
    props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                           D2D1_ALPHA_MODE_PREMULTIPLIED);
    props.dpiX = bmp_dpi;
    props.dpiY = bmp_dpi;

    ID2D1Bitmap* bmp = nullptr;
    UINT32 pitch = width_px * 4;
    HRESULT hr = ctx->target->CreateBitmap(
      D2D1::SizeU(width_px, height_px),
      bgra_pixels, pitch,
      &props,
      &bmp);
    return SUCCEEDED(hr) ? static_cast<void*>(bmp) : nullptr;
  }

  static void d2d_destroy_bitmap(neui_render_ctx_t /*raw*/, void* bitmap)
  {
    if (bitmap)
      static_cast<ID2D1Bitmap*>(bitmap)->Release();
  }

  static void d2d_draw_bitmap(neui_render_ctx_t raw, void* bitmap,
                               float src_x, float src_y, float src_w, float src_h,
                               float dst_x, float dst_y, float dst_w, float dst_h,
                               uint32_t tint)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target || !bitmap) return;

    auto* bmp = static_cast<ID2D1Bitmap*>(bitmap);

    D2D1_SIZE_F logical_size = bmp->GetSize(); // DIPs = logical px (bitmap DPI is set)
    D2D1_RECT_F src_rect;
    if (src_w <= 0.0f || src_h <= 0.0f) {
      src_rect = D2D1::RectF(0.0f, 0.0f, logical_size.width, logical_size.height);
    } else {
      src_rect = D2D1::RectF(src_x, src_y, src_x + src_w, src_y + src_h);
    }

    D2D1_RECT_F dst_rect = D2D1::RectF(dst_x, dst_y, dst_x + dst_w, dst_y + dst_h);

    // Interpolation mode: a bitmap drawn at (approximately) its native size -
    // 1:1 source-to-destination DEVICE pixels - must NOT be linearly resampled,
    // or pixel-exact content (QR symbols, pixel art) gets gray seams from edge
    // blending, especially at fractional DPI like 150%. Compare in device px
    // (not DIPs) so a HiDPI-upscaled photo - same DIP size but more device px -
    // still gets smooth LINEAR scaling. NEAREST only kicks in for genuine
    // 1:1 device blits.
    const float dev_scale = static_cast<float>(ctx->dpi) / 96.0f;
    float src_dev_w, src_dev_h;
    if (src_w <= 0.0f || src_h <= 0.0f) {
      D2D1_SIZE_U px = bmp->GetPixelSize();
      src_dev_w = static_cast<float>(px.width);
      src_dev_h = static_cast<float>(px.height);
    } else {
      FLOAT bdx = 96.0f, bdy = 96.0f;
      bmp->GetDpi(&bdx, &bdy);
      const float bmp_scale = bdx / 96.0f;  // bitmap DIP -> device px
      src_dev_w = src_w * bmp_scale;
      src_dev_h = src_h * bmp_scale;
    }
    const float dst_dev_w = dst_w * dev_scale;
    const float dst_dev_h = dst_h * dev_scale;
    const bool native_1to1 =
      std::fabs(dst_dev_w - src_dev_w) < 0.5f &&
      std::fabs(dst_dev_h - src_dev_h) < 0.5f;
    const D2D1_BITMAP_INTERPOLATION_MODE interp = native_1to1
      ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
      : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;

    if (tint == 0xFFFFFFFFu) {
      // Untinted fast path: byte-for-byte identical to the pre-effect
      // shipping behaviour. Avoids any effect setup so the common case
      // pays no extra cost.
      ctx->target->DrawBitmap(bmp, dst_rect, current_alpha(ctx),
                               interp,
                               src_rect);
      return;
    }

    // Tinted path: route through the D2D1Tint effect (multiplicative).
    // The effect is created lazily per ctx, cached, and reused across draws
    // within a frame - SetInput/SetValue are cheap, no recreate needed.
    if (!ctx->tint_effect) {
      HRESULT hr = ctx->target->CreateEffect(CLSID_D2D1Tint, &ctx->tint_effect);
      if (FAILED(hr) || !ctx->tint_effect) {
        // Effects unavailable; fall back to the untinted draw rather than
        // skipping the layer entirely.
        ctx->target->DrawBitmap(bmp, dst_rect, current_alpha(ctx),
                                 interp,
                                 src_rect);
        return;
      }
    }

    ctx->tint_effect->SetInput(0, bmp);

    // Compose the layer's alpha into the tint colour so the alpha stack
    // still applies to tinted draws. The D2D1Tint effect multiplies its
    // colour vector with the source bitmap's premultiplied pixels; folding
    // current_alpha() into the alpha component yields the same composite
    // factor the untinted path applies via DrawBitmap's opacity arg.
    float tr = ((tint >> 16) & 0xff) / 255.0f;
    float tg = ((tint >>  8) & 0xff) / 255.0f;
    float tb = ((tint >>  0) & 0xff) / 255.0f;
    float ta = ((tint >> 24) & 0xff) / 255.0f * current_alpha(ctx);
    D2D1_VECTOR_4F v = { tr, tg, tb, ta };
    ctx->tint_effect->SetValue(D2D1_TINT_PROP_COLOR, v);

    // DrawImage takes a target offset + source crop but does NOT scale
    // src to dst the way DrawBitmap does. To get the same visual sizing
    // we apply a temporary world transform (current_world ∘ T(dst_x,
    // dst_y) ∘ S(dst_w/src_w, dst_h/src_h)) so the source crop lands
    // inside dst_rect. Pre-multiplying onto ctx->current preserves the
    // outer transform stack (e.g. compound layer rotations).
    float src_w_eff = src_rect.right  - src_rect.left;
    float src_h_eff = src_rect.bottom - src_rect.top;
    bool need_scale = (src_w_eff > 0.0f && src_h_eff > 0.0f
                      && (std::fabs(src_w_eff - dst_w) > 1e-3f
                       || std::fabs(src_h_eff - dst_h) > 1e-3f));

    D2D1::Matrix3x2F saved = ctx->current;
    if (need_scale) {
      D2D1::Matrix3x2F m =
        D2D1::Matrix3x2F::Scale(dst_w / src_w_eff, dst_h / src_h_eff,
                                 D2D1::Point2F(0.0f, 0.0f));
      m = D2D1::Matrix3x2F::Translation(dst_x, dst_y) * m;
      ctx->target->SetTransform(m * saved);
    } else {
      ctx->target->SetTransform(
        D2D1::Matrix3x2F::Translation(dst_x, dst_y) * saved);
    }

    D2D1_POINT_2F origin = D2D1::Point2F(0.0f, 0.0f);
    ctx->target->DrawImage(ctx->tint_effect,
                            &origin,
                            &src_rect,
                            D2D1_INTERPOLATION_MODE_LINEAR,
                            D2D1_COMPOSITE_MODE_SOURCE_OVER);

    ctx->target->SetTransform(saved);
  }

  static void d2d_push_clip(neui_render_ctx_t raw, float x, float y, float w, float h)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target) return;
    ctx->target->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, y + h),
                                     D2D1_ANTIALIAS_MODE_ALIASED);
  }

  static void d2d_pop_clip(neui_render_ctx_t raw)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target) return;
    ctx->target->PopAxisAlignedClip();
  }

  // ---------------------------------------------------------------------------
  // Path API

  // Internal: release any open path/sink (called from begin_path and from
  // the implicit teardown if a path is left around when the context dies).
  static void release_path(D2DContext* ctx)
  {
    if (ctx->sink) {
      if (ctx->figure_open) {
        ctx->sink->EndFigure(D2D1_FIGURE_END_OPEN);
        ctx->figure_open = false;
      }
      if (!ctx->sink_closed) {
        ctx->sink->Close();
      }
      ctx->sink->Release();
      ctx->sink = nullptr;
    }
    if (ctx->path) { ctx->path->Release(); ctx->path = nullptr; }
    ctx->sink_closed = false;
  }

  // Internal: end the figure (if open) and close the sink (if not already
  // closed). After this the path geometry is finalised and can be drawn
  // multiple times via FillGeometry / DrawGeometry, but no more segments
  // can be appended (a new begin_path is required for that).
  static void finalise_path(D2DContext* ctx)
  {
    if (!ctx->sink) return;
    if (ctx->figure_open) {
      ctx->sink->EndFigure(D2D1_FIGURE_END_OPEN);
      ctx->figure_open = false;
    }
    if (!ctx->sink_closed) {
      ctx->sink->Close();
      ctx->sink_closed = true;
    }
  }

  static void d2d_begin_path(neui_render_ctx_t raw)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target) return;

    release_path(ctx);

    ID2D1PathGeometry* path = nullptr;
    if (FAILED(g_factory->CreatePathGeometry(&path))) return;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(path->Open(&sink))) { path->Release(); return; }

    ctx->path        = path;
    ctx->sink        = sink;
    ctx->figure_open = false;
    ctx->sink_closed = false;
    ctx->cursor_pt   = D2D1::Point2F(0.0f, 0.0f);
  }

  static void d2d_move_to(neui_render_ctx_t raw, float x, float y)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->sink || ctx->sink_closed) return;

    if (ctx->figure_open) {
      ctx->sink->EndFigure(D2D1_FIGURE_END_OPEN);
      ctx->figure_open = false;
    }
    D2D1_POINT_2F p = D2D1::Point2F(x, y);
    ctx->sink->BeginFigure(p, D2D1_FIGURE_BEGIN_FILLED);
    ctx->figure_open = true;
    ctx->cursor_pt   = p;
  }

  static void d2d_line_to(neui_render_ctx_t raw, float x, float y)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->sink || ctx->sink_closed) return;

    if (!ctx->figure_open) {
      ctx->sink->BeginFigure(ctx->cursor_pt, D2D1_FIGURE_BEGIN_FILLED);
      ctx->figure_open = true;
    }
    D2D1_POINT_2F p = D2D1::Point2F(x, y);
    ctx->sink->AddLine(p);
    ctx->cursor_pt = p;
  }

  // Append one D2D arc segment. Caller has already opened the figure.
  static void d2d_add_arc_segment(D2DContext* ctx,
                                   float cx, float cy, float radius,
                                   float start_rad, float end_rad)
  {
    D2D1_POINT_2F end = D2D1::Point2F(cx + radius * cosf(end_rad),
                                       cy + radius * sinf(end_rad));
    float sweep = end_rad - start_rad;
    D2D1_SWEEP_DIRECTION dir =
      (sweep >= 0.0f) ? D2D1_SWEEP_DIRECTION_CLOCKWISE
                      : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;
    float abs_sweep = sweep < 0.0f ? -sweep : sweep;
    D2D1_ARC_SIZE size =
      (abs_sweep > 3.14159265f) ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;

    D2D1_ARC_SEGMENT seg = {};
    seg.point          = end;
    seg.size           = D2D1::SizeF(radius, radius);
    seg.rotationAngle  = 0.0f;
    seg.sweepDirection = dir;
    seg.arcSize        = size;
    ctx->sink->AddArc(seg);
    ctx->cursor_pt = end;
  }

  static void d2d_arc(neui_render_ctx_t raw,
                       float cx, float cy, float radius,
                       float start_rad, float end_rad)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->sink || ctx->sink_closed) return;

    D2D1_POINT_2F start = D2D1::Point2F(cx + radius * cosf(start_rad),
                                         cy + radius * sinf(start_rad));

    if (!ctx->figure_open) {
      ctx->sink->BeginFigure(start, D2D1_FIGURE_BEGIN_FILLED);
      ctx->figure_open = true;
    } else {
      // Bridge from current cursor to the arc start.
      ctx->sink->AddLine(start);
    }

    // Direct2D's AddArc renders nothing when the start point and end point
    // coincide, so a full-circle arc (|sweep| ~= 2pi) produces no output.
    // Split such arcs into two halves through the antipodal point so each
    // segment has distinct endpoints.
    const float PI     = 3.14159265358979323846f;
    const float TWO_PI = 6.28318530717958647692f;
    float sweep = end_rad - start_rad;
    float abs_sweep = sweep < 0.0f ? -sweep : sweep;
    if (abs_sweep + 1e-4f >= TWO_PI) {
      float mid_rad = start_rad + (sweep >= 0.0f ? PI : -PI);
      d2d_add_arc_segment(ctx, cx, cy, radius, start_rad, mid_rad);
      d2d_add_arc_segment(ctx, cx, cy, radius, mid_rad,   end_rad);
    } else {
      d2d_add_arc_segment(ctx, cx, cy, radius, start_rad, end_rad);
    }
  }

  static void d2d_close_path(neui_render_ctx_t raw)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->sink || ctx->sink_closed) return;

    if (ctx->figure_open) {
      ctx->sink->EndFigure(D2D1_FIGURE_END_CLOSED);
      ctx->figure_open = false;
    }
  }

  static void d2d_fill_path(neui_render_ctx_t raw, uint32_t argb)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target || !ctx->brush || !ctx->path) return;

    finalise_path(ctx);
    ctx->brush->SetColor(argb_to_color(argb, current_alpha(ctx)));
    ctx->target->FillGeometry(ctx->path, ctx->brush);
  }

  static void d2d_stroke_path(neui_render_ctx_t raw, float stroke_width, uint32_t argb)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target || !ctx->brush || !ctx->path) return;

    finalise_path(ctx);
    ctx->brush->SetColor(argb_to_color(argb, current_alpha(ctx)));
    ctx->target->DrawGeometry(ctx->path, ctx->brush, stroke_width);
  }

  // ---------------------------------------------------------------------------
  // Transform stack

  static void d2d_push_transform(neui_render_ctx_t raw)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx) return;
    ctx->transform_stack.push_back(ctx->current);
  }

  static void d2d_pop_transform(neui_render_ctx_t raw)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || ctx->transform_stack.empty()) return;
    ctx->current = ctx->transform_stack.back();
    ctx->transform_stack.pop_back();
    if (ctx->target) ctx->target->SetTransform(ctx->current);
  }

  static void d2d_translate(neui_render_ctx_t raw, float dx, float dy)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx) return;
    // Post-multiply: child translation applies inside the parent's frame.
    ctx->current = D2D1::Matrix3x2F::Translation(dx, dy) * ctx->current;
    if (ctx->target) ctx->target->SetTransform(ctx->current);
  }

  static void d2d_rotate(neui_render_ctx_t raw, float radians)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx) return;
    // D2D's Matrix3x2F::Rotation takes degrees, not radians.
    float deg = radians * (180.0f / 3.14159265358979323846f);
    ctx->current = D2D1::Matrix3x2F::Rotation(deg, D2D1::Point2F(0.0f, 0.0f)) * ctx->current;
    if (ctx->target) ctx->target->SetTransform(ctx->current);
  }

  static void d2d_scale(neui_render_ctx_t raw, float sx, float sy)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx) return;
    ctx->current = D2D1::Matrix3x2F::Scale(sx, sy, D2D1::Point2F(0.0f, 0.0f)) * ctx->current;
    if (ctx->target) ctx->target->SetTransform(ctx->current);
  }

  // ---------------------------------------------------------------------------
  // Alpha stack

  static void d2d_push_alpha(neui_render_ctx_t raw, float factor)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx) return;
    neui_detail::alpha_stack_push(ctx->alpha_stack, factor);
  }

  static void d2d_pop_alpha(neui_render_ctx_t raw)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx) return;
    neui_detail::alpha_stack_pop(ctx->alpha_stack);
  }

  // ---------------------------------------------------------------------------
  // Font stack

  static void d2d_push_font(neui_render_ctx_t raw,
                             const char* family_utf8,
                             int          weight)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx) return;
    FontState fs;
    if (family_utf8 && *family_utf8) {
      int needed = MultiByteToWideChar(CP_UTF8, 0, family_utf8, -1, nullptr, 0);
      if (needed > 1) {
        fs.family.resize(needed - 1);
        MultiByteToWideChar(CP_UTF8, 0, family_utf8, -1, fs.family.data(), needed);
      }
    }
    fs.weight = weight;
    ctx->font_stack.push_back(std::move(fs));
  }

  static void d2d_pop_font(neui_render_ctx_t raw)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || ctx->font_stack.empty()) return;
    ctx->font_stack.pop_back();
  }

  // ---------------------------------------------------------------------------
  // Gradient fills (NEUI_ASSET_KIND none - immediate-mode brush).

  static D2D1_EXTEND_MODE d2d_extend_mode(neui_gradient_extend_t e)
  {
    switch (e) {
      case NEUI_GRADIENT_EXTEND_REPEAT: return D2D1_EXTEND_MODE_WRAP;
      case NEUI_GRADIENT_EXTEND_MIRROR: return D2D1_EXTEND_MODE_MIRROR;
      default:                          return D2D1_EXTEND_MODE_CLAMP;
    }
  }

  // Build a one-shot gradient brush from `g`. Caller owns the returned brush
  // and must Release() it. The stop colours fold in the alpha stack so a
  // gradient fill respects push_alpha exactly like a solid fill. Returns null
  // on a malformed gradient or allocation failure.
  static ID2D1Brush* d2d_make_gradient_brush(D2DContext* ctx,
                                              const neui_gradient_t* g)
  {
    if (!ctx || !ctx->target || !g || !g->stops || g->stop_count < 2)
      return nullptr;

    const float alpha = current_alpha(ctx);
    std::vector<D2D1_GRADIENT_STOP> stops(g->stop_count);
    for (uint32_t i = 0; i < g->stop_count; ++i) {
      float off = g->stops[i].offset;
      if (off < 0.0f) off = 0.0f; else if (off > 1.0f) off = 1.0f;
      stops[i].position = off;
      stops[i].color    = argb_to_color(g->stops[i].argb, alpha);
    }

    ID2D1GradientStopCollection* coll = nullptr;
    HRESULT hr = ctx->target->CreateGradientStopCollection(
      stops.data(), g->stop_count, D2D1_GAMMA_2_2,
      d2d_extend_mode(g->extend), &coll);
    if (FAILED(hr) || !coll) return nullptr;

    ID2D1Brush* brush = nullptr;
    if (g->kind == NEUI_GRADIENT_RADIAL) {
      ID2D1RadialGradientBrush* rb = nullptr;
      // gradientOriginOffset is the focal point relative to the centre.
      hr = ctx->target->CreateRadialGradientBrush(
        D2D1::RadialGradientBrushProperties(
          D2D1::Point2F(g->start_x, g->start_y),
          D2D1::Point2F(g->end_x - g->start_x, g->end_y - g->start_y),
          g->radius, g->radius),
        coll, &rb);
      brush = rb;
    } else {
      ID2D1LinearGradientBrush* lb = nullptr;
      hr = ctx->target->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(
          D2D1::Point2F(g->start_x, g->start_y),
          D2D1::Point2F(g->end_x,   g->end_y)),
        coll, &lb);
      brush = lb;
    }
    coll->Release();  // the brush holds its own reference
    if (FAILED(hr)) { if (brush) brush->Release(); return nullptr; }
    return brush;
  }

  static void d2d_fill_rect_gradient(neui_render_ctx_t raw,
                                     float x, float y, float w, float h,
                                     const neui_gradient_t* g)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target) return;
    ID2D1Brush* br = d2d_make_gradient_brush(ctx, g);
    if (!br) return;
    ctx->target->FillRectangle(D2D1::RectF(x, y, x + w, y + h), br);
    br->Release();
  }

  static void d2d_fill_path_gradient(neui_render_ctx_t raw,
                                     const neui_gradient_t* g)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target || !ctx->path) return;
    ID2D1Brush* br = d2d_make_gradient_brush(ctx, g);
    if (!br) return;
    finalise_path(ctx);
    ctx->target->FillGeometry(ctx->path, br);
    br->Release();
  }

  // ---------------------------------------------------------------------------
  // Off-screen contexts (NEUI_ASSET_KIND_SURFACE).

  static neui_render_ctx_t d2d_create_offscreen_context(
      uint32_t width_px, uint32_t height_px, float scale)
  {
    if (width_px == 0 || height_px == 0) return nullptr;
    if (!ensure_factory()) return nullptr;
    if (!ensure_device_chain()) return nullptr;
    if (scale <= 0.0f) scale = 1.0f;

    // Build a fresh per-surface ID2D1DeviceContext. The off-screen target
    // is a D2D 1.1 GPU bitmap (D2D1_BITMAP_OPTIONS_TARGET). Pixels are
    // GPU-only - read_pixels_bgra copies them into a lazy staging bitmap
    // (created in read_pixels_bgra below, with CANNOT_DRAW | CPU_READ)
    // and Maps the staging bitmap to memcpy the bytes out. We no longer
    // back the target with a WIC bitmap because TARGET bitmaps in 1.1
    // are GPU textures - the WIC source would only ever hold the initial
    // (zero) content.
    ID2D1DeviceContext* dc = nullptr;
    HRESULT hr = g_d2d_device->CreateDeviceContext(
      D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
    if (FAILED(hr) || !dc) return nullptr;
    dc->SetDpi(scale * 96.0f, scale * 96.0f);

    D2D1_BITMAP_PROPERTIES1 bp = {};
    bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
    bp.pixelFormat   = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                          D2D1_ALPHA_MODE_PREMULTIPLIED);
    bp.dpiX = scale * 96.0f;
    bp.dpiY = scale * 96.0f;
    ID2D1Bitmap1* target_bmp = nullptr;
    hr = dc->CreateBitmap(D2D1::SizeU(width_px, height_px),
                           nullptr, 0u, &bp, &target_bmp);
    if (FAILED(hr) || !target_bmp) { dc->Release(); return nullptr; }
    dc->SetTarget(target_bmp);

    ID2D1SolidColorBrush* brush = nullptr;
    hr = dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush);
    if (FAILED(hr)) {
      target_bmp->Release(); dc->Release();
      return nullptr;
    }

    auto* ctx = new D2DContext();
    ctx->target       = dc;
    ctx->back_buffer  = target_bmp;
    ctx->brush        = brush;
    ctx->surface_w_px = width_px;
    ctx->surface_h_px = height_px;
    ctx->dpi          = static_cast<uint32_t>(scale * 96.0f + 0.5f);
    // swap_chain stays null - this signals "off-screen" to the rest of
    // the backend (resize / Present / device-loss are no-ops).
    return ctx;
  }

  static bool d2d_read_pixels_bgra(neui_render_ctx_t raw, uint8_t* out_bgra)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target || !ctx->back_buffer || !out_bgra) return false;
    if (ctx->surface_w_px == 0 || ctx->surface_h_px == 0) return false;

    // Lazy-allocate a staging bitmap the first time we read back this
    // surface. CANNOT_DRAW | CPU_READ flags mean the bitmap can be
    // Map()-ed to system memory but not used as a draw target - exactly
    // what we want for readback. Reused across subsequent reads of the
    // same surface size.
    if (!ctx->staging_bitmap) {
      D2D1_BITMAP_PROPERTIES1 sp = {};
      sp.bitmapOptions = D2D1_BITMAP_OPTIONS_CANNOT_DRAW
                       | D2D1_BITMAP_OPTIONS_CPU_READ;
      sp.pixelFormat   = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                            D2D1_ALPHA_MODE_PREMULTIPLIED);
      // Staging DPI doesn't affect the byte readback but we keep it
      // consistent with the target so any future GetSize calls stay sane.
      sp.dpiX = static_cast<float>(ctx->dpi);
      sp.dpiY = static_cast<float>(ctx->dpi);
      HRESULT hr = ctx->target->CreateBitmap(
        D2D1::SizeU(ctx->surface_w_px, ctx->surface_h_px),
        nullptr, 0u, &sp, &ctx->staging_bitmap);
      if (FAILED(hr) || !ctx->staging_bitmap) return false;
    }

    // GPU -> CPU copy. CopyFromBitmap on a CPU_READ staging bitmap is
    // the D2D 1.1 idiom for offscreen readback.
    D2D1_POINT_2U dst = D2D1::Point2U(0, 0);
    D2D1_RECT_U   src = D2D1::RectU(0, 0, ctx->surface_w_px, ctx->surface_h_px);
    HRESULT hr = ctx->staging_bitmap->CopyFromBitmap(&dst, ctx->back_buffer, &src);
    if (FAILED(hr)) return false;

    D2D1_MAPPED_RECT mapped = {};
    hr = ctx->staging_bitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped);
    if (FAILED(hr) || !mapped.bits) return false;

    const UINT row_bytes = ctx->surface_w_px * 4u;
    for (UINT y = 0; y < ctx->surface_h_px; ++y) {
      std::memcpy(out_bgra + y * row_bytes,
                   mapped.bits + y * mapped.pitch,
                   row_bytes);
    }
    ctx->staging_bitmap->Unmap();
    return true;
  }

  // ---------------------------------------------------------------------------

  static neui_render_backend_t backend = {
    NEUI_VERSION,
    d2d_create_context,
    d2d_destroy_context,
    d2d_resize,
    d2d_begin_frame,
    d2d_end_frame,
    d2d_fill_rect,
    d2d_draw_rect,
    d2d_get_scale_factor,
    d2d_update_dpi,
    d2d_draw_text,
    d2d_measure_text,
    d2d_push_clip,
    d2d_pop_clip,
    d2d_create_bitmap,
    d2d_destroy_bitmap,
    d2d_draw_bitmap,
    d2d_begin_path,
    d2d_move_to,
    d2d_line_to,
    d2d_arc,
    d2d_close_path,
    d2d_fill_path,
    d2d_stroke_path,
    d2d_push_transform,
    d2d_pop_transform,
    d2d_translate,
    d2d_rotate,
    d2d_scale,
    d2d_get_context_generation,
    d2d_push_alpha,
    d2d_pop_alpha,
    d2d_push_font,
    d2d_pop_font,
    d2d_create_offscreen_context,
    d2d_read_pixels_bgra,
    d2d_register_font,
    d2d_register_font_file,
    d2d_unregister_font,
    d2d_fill_rect_gradient,
    d2d_fill_path_gradient,
  };

  neui_render_backend_t* get_backend() { return &backend; }

} // namespace neui_d2d_backend
