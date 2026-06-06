#pragma once

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <oleidl.h>
#include <objbase.h>
#include <shlobj.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../clipboard_item.h"
#include "clipboard_format_html_win32.h"
#include "clipboard_format_urilist_win32.h"
#include "dnd_target_win32.h"   // dnd_ensure_ole_initialized + dnd_action_to_dropeffect

// Drag-source side of NEUI_API_DND. Mirror of dnd_target_win32.h.
//
// Wraps a DataItem as an IDataObject + IDropSource pair and runs
// DoDragDrop on the calling thread. Formats are pre-encoded at
// construction so GetData is a straightforward HGLOBAL memcpy.

#include "../../../include/neui/d/dnd.h"

namespace neui_detail
{
  // -------------------------------------------------------------------------
  // Pre-encoded format entry: one CLIPFORMAT + its OS-native byte payload.

  struct DragSourceFormat
  {
    CLIPFORMAT            cf;
    std::vector<uint8_t>  bytes;        // payload for HGLOBAL
    bool                  is_hdrop = false;  // CF_HDROP needs a special HGLOBAL layout
  };

  // Build a snapshot of every MIME on `item` as the matching CF_* + bytes.
  inline std::vector<DragSourceFormat>
  drag_source_snapshot(const DataItem& item)
  {
    std::vector<DragSourceFormat> out;

    // "First wins": a DataItem holding both text/plain and
    // text/plain;charset=utf-8 must not emit two CF_UNICODETEXT entries.
    auto has_cf = [&](CLIPFORMAT cf) {
      for (auto& e : out) if (e.cf == cf) return true;
      return false;
    };

    // UTF-8 -> UTF-16 CF_UNICODETEXT entry (one trailing wchar_t L'\0').
    auto push_unicode_text = [&](const char* p, int char_len) {
      int wlen = (char_len > 0)
                   ? MultiByteToWideChar(CP_UTF8, 0, p, char_len, nullptr, 0)
                   : 0;
      DragSourceFormat f;
      f.cf = CF_UNICODETEXT;
      f.bytes.assign((static_cast<size_t>(wlen) + 1) * sizeof(wchar_t), 0);
      if (wlen > 0) {
        MultiByteToWideChar(CP_UTF8, 0, p, char_len,
                            reinterpret_cast<wchar_t*>(f.bytes.data()), wlen);
      }
      out.push_back(std::move(f));
    };

    // Non-file URIs found in text/uri-list. Published as a CF_UNICODETEXT
    // fallback after the loop unless an explicit text payload claimed it.
    std::vector<std::string> other_uris;

    item.for_each_format([&](const std::string& mime,
                              const std::vector<uint8_t>& bytes) {
      // text/plain;charset=utf-8 -> CF_UNICODETEXT
      if (mime == "text/plain;charset=utf-8" || mime == "text/plain") {
        if (has_cf(CF_UNICODETEXT)) return;  // first wins
        // bytes are UTF-8 (with or without trailing null).
        const char* p = reinterpret_cast<const char*>(bytes.data());
        int blen = static_cast<int>(bytes.size());
        bool has_null = (blen > 0 && p[blen - 1] == 0);
        push_unicode_text(p, has_null ? (blen - 1) : blen);
        return;
      }

      // text/html -> CF_HTML
      if (mime == "text/html") {
        UINT cf = clipboard_cf_html_format();
        if (!cf) return;
        auto encoded = clipboard_encode_cf_html(bytes.data(),
                                                 static_cast<uint32_t>(bytes.size()));
        DragSourceFormat f;
        f.cf    = static_cast<CLIPFORMAT>(cf);
        f.bytes = std::move(encoded);
        out.push_back(std::move(f));
        return;
      }

      // text/uri-list -> CF_HDROP (DROPFILES + double-null-terminated wide path list)
      if (mime == "text/uri-list") {
        // We can't store the HGLOBAL directly (it's not a byte buffer
        // shape). Pre-render the DROPFILES bytes here and mark this
        // entry so GetData allocates a movable HGLOBAL of the same
        // layout (the same bytes work either way).
        auto uris = urilist_parse(bytes.data(), bytes.size());
        std::vector<std::wstring> paths;
        paths.reserve(uris.size());
        for (auto& u : uris) {
          auto wp = urilist_uri_to_path(u);
          if (!wp.empty()) paths.push_back(std::move(wp));
          else             other_uris.push_back(u);  // http://, mailto:, ...
        }
        if (paths.empty()) return;  // non-file URIs handled after the loop

        size_t chars = 1;  // trailing extra null
        for (auto& w : paths) chars += w.size() + 1;
        size_t total = sizeof(DROPFILES) + chars * sizeof(wchar_t);

        DragSourceFormat f;
        f.cf       = CF_HDROP;
        f.is_hdrop = true;
        f.bytes.assign(total, 0);
        auto* df = reinterpret_cast<DROPFILES*>(f.bytes.data());
        df->pFiles = sizeof(DROPFILES);
        df->pt.x = 0;
        df->pt.y = 0;
        df->fNC = FALSE;
        df->fWide = TRUE;
        auto* w = reinterpret_cast<wchar_t*>(f.bytes.data() + sizeof(DROPFILES));
        for (auto& path : paths) {
          std::memcpy(w, path.data(), path.size() * sizeof(wchar_t));
          w += path.size();
          *w++ = L'\0';
        }
        *w = L'\0';
        out.push_back(std::move(f));
        return;
      }

      // Anything else: register the MIME as a clipboard format name and
      // carry the bytes through unchanged. Matches the drop-target
      // enumeration on the receiving side.
      if (mime.find('/') == std::string::npos) return;
      UINT cf = RegisterClipboardFormatA(mime.c_str());
      if (!cf) return;
      DragSourceFormat f;
      f.cf    = static_cast<CLIPFORMAT>(cf);
      f.bytes = bytes;
      out.push_back(std::move(f));
    });

    // Non-file URIs (http://, mailto:, ...) can't ride CF_HDROP. Publish
    // them as CF_UNICODETEXT (joined with CRLF) so browsers / link bars /
    // any text-aware receiver still get the payload - unless an explicit
    // text/plain already claimed that format.
    if (!other_uris.empty() && !has_cf(CF_UNICODETEXT)) {
      std::string joined;
      for (auto& u : other_uris) {
        if (!joined.empty()) joined += "\r\n";
        joined += u;
      }
      push_unicode_text(joined.data(), static_cast<int>(joined.size()));
    }
    return out;
  }

  // -------------------------------------------------------------------------
  // IEnumFORMATETC walker over a fixed list of CLIPFORMATs.

  class EnumFORMATETCImpl : public IEnumFORMATETC
  {
  public:
    EnumFORMATETCImpl(std::vector<CLIPFORMAT> formats, ULONG start = 0)
      : _formats(std::move(formats)), _index(start), _ref(1) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
      if (!ppv) return E_POINTER;
      if (riid == IID_IUnknown || riid == IID_IEnumFORMATETC) {
        *ppv = static_cast<IEnumFORMATETC*>(this);
        AddRef();
        return S_OK;
      }
      *ppv = nullptr;
      return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override
    { return InterlockedIncrement(&_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
      LONG r = InterlockedDecrement(&_ref);
      if (r == 0) delete this;
      return static_cast<ULONG>(r);
    }

    STDMETHODIMP Next(ULONG celt, FORMATETC* rgelt, ULONG* pceltFetched) override
    {
      ULONG fetched = 0;
      while (fetched < celt && _index < _formats.size()) {
        rgelt[fetched] = make_etc(_formats[_index]);
        ++fetched;
        ++_index;
      }
      if (pceltFetched) *pceltFetched = fetched;
      return (fetched == celt) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG celt) override
    {
      ULONG remaining = static_cast<ULONG>(_formats.size() - _index);
      if (celt > remaining) { _index = _formats.size(); return S_FALSE; }
      _index += celt;
      return S_OK;
    }
    STDMETHODIMP Reset() override { _index = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumFORMATETC** ppEnum) override
    {
      if (!ppEnum) return E_POINTER;
      *ppEnum = new EnumFORMATETCImpl(_formats, static_cast<ULONG>(_index));
      return S_OK;
    }

  private:
    static FORMATETC make_etc(CLIPFORMAT cf)
    {
      FORMATETC fe = {};
      fe.cfFormat = cf;
      fe.ptd      = nullptr;
      fe.dwAspect = DVASPECT_CONTENT;
      fe.lindex   = -1;
      fe.tymed    = TYMED_HGLOBAL;
      return fe;
    }

    std::vector<CLIPFORMAT> _formats;
    size_t                  _index;
    LONG                    _ref;
  };

  // -------------------------------------------------------------------------
  // Read-only IDataObject backed by a pre-encoded format snapshot.

  class DataObjectImpl : public IDataObject
  {
  public:
    explicit DataObjectImpl(const DataItem& item)
      : _formats(drag_source_snapshot(item)), _ref(1) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
      if (!ppv) return E_POINTER;
      if (riid == IID_IUnknown || riid == IID_IDataObject) {
        *ppv = static_cast<IDataObject*>(this);
        AddRef();
        return S_OK;
      }
      *ppv = nullptr;
      return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override
    { return InterlockedIncrement(&_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
      LONG r = InterlockedDecrement(&_ref);
      if (r == 0) delete this;
      return static_cast<ULONG>(r);
    }

    STDMETHODIMP GetData(FORMATETC* pFormatEtcIn, STGMEDIUM* pMedium) override
    {
      if (!pFormatEtcIn || !pMedium) return E_POINTER;
      std::memset(pMedium, 0, sizeof(*pMedium));
      if (pFormatEtcIn->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;

      const DragSourceFormat* f = find(pFormatEtcIn->cfFormat);
      if (!f) return DV_E_FORMATETC;

      // The Shell's IDragSourceHelper writes some of its bookkeeping
      // formats via TYMED_ISTREAM and reads them back the same way; the
      // historic neui formats round-trip through TYMED_HGLOBAL. Honour
      // whichever the caller's tymed bitmask allows, HGLOBAL first.
      if (pFormatEtcIn->tymed & TYMED_HGLOBAL) {
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, f->bytes.size());
        if (!hg) return E_OUTOFMEMORY;
        void* dst = GlobalLock(hg);
        if (!dst) { GlobalFree(hg); return E_OUTOFMEMORY; }
        std::memcpy(dst, f->bytes.data(), f->bytes.size());
        GlobalUnlock(hg);
        pMedium->tymed   = TYMED_HGLOBAL;
        pMedium->hGlobal = hg;
        return S_OK;
      }
      if (pFormatEtcIn->tymed & TYMED_ISTREAM) {
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, f->bytes.size());
        if (!hg) return E_OUTOFMEMORY;
        void* dst = GlobalLock(hg);
        if (!dst) { GlobalFree(hg); return E_OUTOFMEMORY; }
        std::memcpy(dst, f->bytes.data(), f->bytes.size());
        GlobalUnlock(hg);
        IStream* stream = nullptr;
        HRESULT hr = CreateStreamOnHGlobal(hg, /*fDeleteOnRelease=*/TRUE,
                                            &stream);
        if (FAILED(hr) || !stream) {
          GlobalFree(hg);
          return FAILED(hr) ? hr : E_OUTOFMEMORY;
        }
        pMedium->tymed = TYMED_ISTREAM;
        pMedium->pstm  = stream;
        return S_OK;
      }
      return DV_E_TYMED;
    }
    STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override
    { return DATA_E_FORMATETC; }
    STDMETHODIMP QueryGetData(FORMATETC* pFormatEtc) override
    {
      if (!pFormatEtc) return E_POINTER;
      if (!(pFormatEtc->tymed & (TYMED_HGLOBAL | TYMED_ISTREAM)))
        return DV_E_TYMED;
      if (pFormatEtc->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
      return find(pFormatEtc->cfFormat) ? S_OK : DV_E_FORMATETC;
    }
    STDMETHODIMP GetCanonicalFormatEtc(FORMATETC* /*pIn*/, FORMATETC* pOut) override
    {
      if (!pOut) return E_POINTER;
      std::memset(pOut, 0, sizeof(*pOut));
      pOut->ptd = nullptr;
      return DATA_S_SAMEFORMATETC;
    }
    // IDragSourceHelper::InitializeFromBitmap writes the drag-image bits
    // (CFSTR_DRAGIMAGEBITS) + drop description (CFSTR_DROPDESCRIPTION) +
    // CFSTR_INDRAGLOOP into the IDataObject via SetData. Some of these
    // formats arrive on TYMED_HGLOBAL, others on TYMED_ISTREAM; refusing
    // either one makes InitializeFromBitmap fail (0x80040069) and the
    // Shell silently drops the drag preview. We accept both, read the
    // bytes out, and stash them as a DragSourceFormat. The newly-added
    // formats automatically show up in EnumFormatEtc, which is how
    // Shell-aware drop targets discover them.
    STDMETHODIMP SetData(FORMATETC* pFormatEtc, STGMEDIUM* pMedium,
                          BOOL fRelease) override
    {
      if (!pFormatEtc || !pMedium) return E_POINTER;

      std::vector<uint8_t> bytes;
      if (pMedium->tymed == TYMED_HGLOBAL && pMedium->hGlobal) {
        SIZE_T sz  = GlobalSize(pMedium->hGlobal);
        void*  src = GlobalLock(pMedium->hGlobal);
        if (!src) return E_FAIL;
        bytes.assign(reinterpret_cast<uint8_t*>(src),
                      reinterpret_cast<uint8_t*>(src) + sz);
        GlobalUnlock(pMedium->hGlobal);
      } else if (pMedium->tymed == TYMED_ISTREAM && pMedium->pstm) {
        // Slurp the whole stream into bytes. Reset position first - the
        // Shell sometimes leaves the read cursor at the end after its
        // own bookkeeping.
        LARGE_INTEGER zero = {};
        pMedium->pstm->Seek(zero, STREAM_SEEK_SET, nullptr);
        uint8_t buf[64 * 1024];
        ULONG read = 0;
        do {
          HRESULT hr = pMedium->pstm->Read(buf, sizeof(buf), &read);
          if (FAILED(hr)) return hr;
          bytes.insert(bytes.end(), buf, buf + read);
        } while (read == sizeof(buf));
      } else {
        return DV_E_TYMED;
      }

      // Replace existing entry of the same format, else append.
      bool replaced = false;
      for (auto& f : _formats) {
        if (f.cf == pFormatEtc->cfFormat) {
          f.bytes    = std::move(bytes);
          f.is_hdrop = false;
          replaced   = true;
          break;
        }
      }
      if (!replaced) {
        DragSourceFormat f;
        f.cf       = pFormatEtc->cfFormat;
        f.bytes    = std::move(bytes);
        f.is_hdrop = false;
        _formats.push_back(std::move(f));
      }

      if (fRelease) ReleaseStgMedium(pMedium);
      return S_OK;
    }
    STDMETHODIMP EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC** ppEnum) override
    {
      if (!ppEnum) return E_POINTER;
      if (dwDirection != DATADIR_GET) return E_NOTIMPL;
      std::vector<CLIPFORMAT> cfs;
      cfs.reserve(_formats.size());
      for (auto& f : _formats) cfs.push_back(f.cf);
      *ppEnum = new EnumFORMATETCImpl(std::move(cfs));
      return S_OK;
    }
    STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override
    { return OLE_E_ADVISENOTSUPPORTED; }
    STDMETHODIMP DUnadvise(DWORD) override
    { return OLE_E_ADVISENOTSUPPORTED; }
    STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) override
    { return OLE_E_ADVISENOTSUPPORTED; }

  private:
    const DragSourceFormat* find(CLIPFORMAT cf) const
    {
      for (auto& f : _formats) if (f.cf == cf) return &f;
      return nullptr;
    }

    std::vector<DragSourceFormat> _formats;
    LONG                          _ref;
  };

  // -------------------------------------------------------------------------
  // Minimal IDropSource. Default OS cursors; cancel on escape; drop on
  // left-button release.

  class DropSourceImpl : public IDropSource
  {
  public:
    DropSourceImpl() : _ref(1) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
      if (!ppv) return E_POINTER;
      if (riid == IID_IUnknown || riid == IID_IDropSource) {
        *ppv = static_cast<IDropSource*>(this);
        AddRef();
        return S_OK;
      }
      *ppv = nullptr;
      return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override
    { return InterlockedIncrement(&_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
      LONG r = InterlockedDecrement(&_ref);
      if (r == 0) delete this;
      return static_cast<ULONG>(r);
    }

    STDMETHODIMP QueryContinueDrag(BOOL fEscapePressed, DWORD grfKeyState) override
    {
      if (fEscapePressed) return DRAGDROP_S_CANCEL;
      if (!(grfKeyState & MK_LBUTTON)) return DRAGDROP_S_DROP;
      return S_OK;
    }
    STDMETHODIMP GiveFeedback(DWORD /*dwEffect*/) override
    {
      return DRAGDROP_S_USEDEFAULTCURSORS;
    }

  private:
    LONG _ref;
  };

  // -------------------------------------------------------------------------
  // Optional drag-preview image. preview_hbitmap may be null (no preview,
  // OS default cursor). When supplied it's a 32-bit BGRA pre-multiplied
  // top-down DIB (whatever w32_make_drag_hbitmap built); IDragSourceHelper
  // owns it after InitializeFromBitmap, so the caller does NOT delete it
  // after a successful call. hot_x / hot_y in image pixel coords.

  struct DragPreviewW32
  {
    HBITMAP hbitmap = nullptr;
    int     width   = 0;
    int     height  = 0;
    int     hot_x   = 0;
    int     hot_y   = 0;
  };

  // -------------------------------------------------------------------------
  // Entry point used by platform_win32.cpp and hosts/win32/widgets.cpp.

  inline uint32_t platform_dnd_begin_drag_w32(void* /*native_handle*/,
                                               DataItem* item,
                                               uint32_t allowed_actions,
                                               const DragPreviewW32* preview = nullptr)
  {
    if (!item) return 0;
    if (!allowed_actions) return 0;
    dnd_ensure_ole_initialized();

    DataObjectImpl* obj    = new DataObjectImpl(*item);
    DropSourceImpl* source = new DropSourceImpl();

    // Attach drag image BEFORE DoDragDrop. IDragSourceHelper stores the
    // bitmap reference via the data object's stored IDataObject side
    // channel (CFSTR_DRAGIMAGEBITS / CFSTR_DROPDESCRIPTION) and AppKit-
    // style "follow the cursor" rendering is done by the shell.
    //
    // Ownership: per Shell docs, IDragSourceHelper takes ownership of
    // hbmpDragImage on a successful InitializeFromBitmap call; we must
    // not DeleteObject it afterwards. On failure ownership stays with
    // the caller - we DeleteObject in that case.
    bool helper_owns_bitmap = false;
    if (preview && preview->hbitmap) {
      IDragSourceHelper* helper = nullptr;
      HRESULT hh = CoCreateInstance(CLSID_DragDropHelper, nullptr,
                                     CLSCTX_INPROC_SERVER,
                                     IID_IDragSourceHelper,
                                     reinterpret_cast<void**>(&helper));
      if (SUCCEEDED(hh) && helper) {
        SHDRAGIMAGE sdi = {};
        sdi.sizeDragImage.cx = preview->width;
        sdi.sizeDragImage.cy = preview->height;
        sdi.ptOffset.x       = preview->hot_x;
        sdi.ptOffset.y       = preview->hot_y;
        sdi.hbmpDragImage    = preview->hbitmap;
        // CLR_NONE: rely on the bitmap's own alpha channel (it's premul
        // BGRA); no per-pixel colour key masking.
        sdi.crColorKey       = CLR_NONE;
        HRESULT hr2 = helper->InitializeFromBitmap(&sdi, obj);
        if (SUCCEEDED(hr2)) helper_owns_bitmap = true;
        helper->Release();
      }
    }

    DWORD effects = dnd_action_to_dropeffect(allowed_actions);
    DWORD result  = 0;
    HRESULT hr = DoDragDrop(obj, source, effects, &result);
    obj->Release();
    source->Release();

    if (preview && preview->hbitmap && !helper_owns_bitmap) {
      DeleteObject(preview->hbitmap);
    }

    if (hr != DRAGDROP_S_DROP) return 0;
    if (result & DROPEFFECT_COPY) return NEUI_DND_ACTION_COPY;
    if (result & DROPEFFECT_MOVE) return NEUI_DND_ACTION_MOVE;
    if (result & DROPEFFECT_LINK) return NEUI_DND_ACTION_LINK;
    return 0;
  }

  // -------------------------------------------------------------------------
  // Build a 32-bit pre-multiplied BGRA top-down DIB section from a raw
  // BGRA8 pixel buffer (same layout AssetEntry::pixels stores). Returns
  // null on failure. Caller passes ownership to platform_dnd_begin_drag_w32
  // via DragPreviewW32::hbitmap; if it isn't used in a successful
  // InitializeFromBitmap, the caller must DeleteObject it.
  inline HBITMAP w32_make_drag_hbitmap(const uint8_t* bgra_premul,
                                         uint32_t w_px, uint32_t h_px)
  {
    if (!bgra_premul || w_px == 0 || h_px == 0) return nullptr;
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = static_cast<LONG>(w_px);
    bmi.bmiHeader.biHeight      = -static_cast<LONG>(h_px); // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* dst = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP hbm = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &dst,
                                     nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!hbm || !dst) {
      if (hbm) DeleteObject(hbm);
      return nullptr;
    }
    std::memcpy(dst, bgra_premul, static_cast<size_t>(w_px) * h_px * 4);
    return hbm;
  }

} // namespace neui_detail

#endif // _WIN32
