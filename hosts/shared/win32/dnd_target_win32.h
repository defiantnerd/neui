#pragma once

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <oleidl.h>
#include <objbase.h>
#include <cstdint>
#include <string>
#include <vector>

#include "../clipboard_item.h"
#include "clipboard_format_html_win32.h"
#include "clipboard_format_urilist_win32.h"

// IDropTarget implementation for the Win32 native + crossplatform hosts.
// Forwards DragEnter / DragOver / DragLeave / Drop into a Session via a
// callback table the host fills in - the helper stays platform-glue-only
// and doesn't know about the host's Session type.

namespace neui_detail
{
  // Callback table the host fills in to receive translated DnD events.
  // Each call returns the OS DROPEFFECT_* to apply to the cursor.
  struct DndDispatchSeam
  {
    void*    session_ptr      = nullptr;
    uint32_t frame_widget_id  = 0;

    // Called when the cursor enters / moves over the frame during a drag.
    // formats is a borrowed list of MIME strings active for the drag.
    // Returns the accepted action (neui_dnd_action_t bitmask).
    using EnterFn = uint32_t (*)(void* session_ptr, uint32_t frame_widget_id,
                                  int frame_x, int frame_y,
                                  const char* const* formats,
                                  uint32_t formats_count,
                                  uint32_t suggested_action,
                                  uint32_t buttonmap);
    using MoveFn  = EnterFn;
    using LeaveFn = void (*)(void* session_ptr);
    // Drop: same as enter/move but receives the materialised DataItem.
    using DropFn  = uint32_t (*)(void* session_ptr, uint32_t frame_widget_id,
                                  int frame_x, int frame_y,
                                  const char* const* formats,
                                  uint32_t formats_count,
                                  uint32_t suggested_action,
                                  uint32_t buttonmap,
                                  DataItem* drop_item);

    EnterFn on_enter = nullptr;
    MoveFn  on_move  = nullptr;
    LeaveFn on_leave = nullptr;
    DropFn  on_drop  = nullptr;
  };

  // Extract MIME names + raw byte payloads from an IDataObject. Probes
  // CF_UNICODETEXT, CF_HTML, CF_HDROP, then enumerates remaining
  // RegisterClipboardFormatA-style names (anything containing '/'). The
  // returned DataItem holds the bytes for the duration of the drop
  // callback.
  inline void dnd_pull_item_from_data_object(IDataObject* obj, DataItem& item)
  {
    if (!obj) return;

    auto pull = [&](UINT cf, const std::string& mime,
                    auto on_bytes) {
      FORMATETC fe = { (CLIPFORMAT)cf, nullptr, DVASPECT_CONTENT, -1,
                       TYMED_HGLOBAL };
      STGMEDIUM sm = {};
      if (obj->GetData(&fe, &sm) != S_OK) return;
      if (sm.tymed == TYMED_HGLOBAL && sm.hGlobal) {
        SIZE_T sz = GlobalSize(sm.hGlobal);
        auto* p = GlobalLock(sm.hGlobal);
        if (p && sz > 0) {
          on_bytes(p, sz, mime);
        }
        if (p) GlobalUnlock(sm.hGlobal);
      }
      ReleaseStgMedium(&sm);
    };

    // CF_UNICODETEXT -> text/plain (UTF-16 to UTF-8, include null).
    pull(CF_UNICODETEXT, "text/plain;charset=utf-8",
         [&](void* p, SIZE_T /*sz*/, const std::string& mime) {
           auto* w = static_cast<const wchar_t*>(p);
           int n = WideCharToMultiByte(CP_UTF8, 0, w, -1,
                                        nullptr, 0, nullptr, nullptr);
           if (n > 0) {
             std::vector<char> buf(static_cast<size_t>(n));
             WideCharToMultiByte(CP_UTF8, 0, w, -1, buf.data(), n,
                                 nullptr, nullptr);
             item.set_format(mime, buf.data(), static_cast<uint32_t>(n));
           }
         });

    // CF_HTML -> text/html (extract fragment).
    if (UINT cf_html = clipboard_cf_html_format()) {
      pull(cf_html, "text/html",
           [&](void* p, SIZE_T sz, const std::string& mime) {
             auto frag = clipboard_decode_cf_html(p, sz);
             if (!frag.empty())
               item.set_format(mime, frag.data(),
                               static_cast<uint32_t>(frag.size()));
           });
    }

    // CF_HDROP -> text/uri-list.
    {
      FORMATETC fe = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
      STGMEDIUM sm = {};
      if (obj->GetData(&fe, &sm) == S_OK) {
        if (sm.tymed == TYMED_HGLOBAL && sm.hGlobal) {
          HDROP hdrop = static_cast<HDROP>(GlobalLock(sm.hGlobal));
          if (hdrop) {
            auto bytes = urilist_from_hdrop(hdrop);
            if (!bytes.empty())
              item.set_format("text/uri-list", bytes.data(),
                              static_cast<uint32_t>(bytes.size()));
            GlobalUnlock(sm.hGlobal);
          }
        }
        ReleaseStgMedium(&sm);
      }
    }

    // Enumerate remaining MIME-like registered formats.
    IEnumFORMATETC* en = nullptr;
    if (obj->EnumFormatEtc(DATADIR_GET, &en) == S_OK && en) {
      FORMATETC fes[16];
      ULONG fetched = 0;
      while (en->Next(16, fes, &fetched) == S_OK && fetched > 0) {
        for (ULONG i = 0; i < fetched; ++i) {
          UINT cf = fes[i].cfFormat;
          if (cf == CF_UNICODETEXT || cf == CF_TEXT || cf == CF_OEMTEXT ||
              cf == CF_HDROP) continue;
          if (clipboard_cf_html_format() && cf == clipboard_cf_html_format())
            continue;
          char name[256];
          int len = GetClipboardFormatNameA(cf, name, sizeof(name));
          if (len <= 0) continue;
          std::string mime(name, name + len);
          if (mime.find('/') == std::string::npos) continue;
          if (item.has_format(mime)) continue;
          pull(cf, mime,
               [&](void* p, SIZE_T sz, const std::string& m) {
                 item.set_format(m, p, static_cast<uint32_t>(sz));
               });
        }
        fetched = 0;
      }
      en->Release();
    }
  }

  // Translate a Win32 KEY/MOUSE state bitmap (grfKeyState) to NEUI_MK_*
  // flags. Matches what platform_win32.cpp does for mouse events.
  inline uint32_t dnd_buttonmap_from_keystate(DWORD grfKeyState)
  {
    return static_cast<uint32_t>(grfKeyState) & 0xFFu;
  }

  inline uint32_t dnd_action_to_dropeffect(uint32_t action)
  {
    // neui_dnd_action_t numeric values align with DROPEFFECT_*:
    // COPY=1, MOVE=2, LINK=4 - same as DROPEFFECT_COPY/MOVE/LINK.
    return action & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
  }

  inline uint32_t dnd_dropeffect_suggested(DWORD effects)
  {
    // The OS-suggested effect; pick whichever bit the OS prefers. COPY
    // takes priority since most external drags are copy-by-default.
    if (effects & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    if (effects & DROPEFFECT_MOVE) return DROPEFFECT_MOVE;
    if (effects & DROPEFFECT_LINK) return DROPEFFECT_LINK;
    return 0;
  }

  class DropTargetImpl : public IDropTarget
  {
  public:
    DropTargetImpl(HWND hwnd, DndDispatchSeam seam)
      : _hwnd(hwnd), _seam(seam), _ref(1) {}

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
      if (!ppv) return E_POINTER;
      if (riid == IID_IUnknown || riid == IID_IDropTarget) {
        *ppv = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
      }
      *ppv = nullptr;
      return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override
    {
      return InterlockedIncrement(&_ref);
    }
    STDMETHODIMP_(ULONG) Release() override
    {
      LONG r = InterlockedDecrement(&_ref);
      if (r == 0) delete this;
      return (ULONG)r;
    }

    // IDropTarget
    STDMETHODIMP DragEnter(IDataObject* pDataObj, DWORD grfKeyState,
                            POINTL pt, DWORD* pdwEffect) override
    {
      cache_formats(pDataObj);
      uint32_t suggested = dnd_dropeffect_suggested(*pdwEffect);
      int x, y;
      screen_to_client(pt, x, y);
      uint32_t accepted = 0;
      if (_seam.on_enter) {
        accepted = _seam.on_enter(_seam.session_ptr, _seam.frame_widget_id,
                                   x, y,
                                   _format_ptrs.data(),
                                   static_cast<uint32_t>(_format_ptrs.size()),
                                   suggested,
                                   dnd_buttonmap_from_keystate(grfKeyState));
      }
      *pdwEffect = accepted ? dnd_action_to_dropeffect(accepted)
                            : DROPEFFECT_NONE;
      return S_OK;
    }

    STDMETHODIMP DragOver(DWORD grfKeyState, POINTL pt,
                           DWORD* pdwEffect) override
    {
      uint32_t suggested = dnd_dropeffect_suggested(*pdwEffect);
      int x, y;
      screen_to_client(pt, x, y);
      uint32_t accepted = 0;
      if (_seam.on_move) {
        accepted = _seam.on_move(_seam.session_ptr, _seam.frame_widget_id,
                                  x, y,
                                  _format_ptrs.data(),
                                  static_cast<uint32_t>(_format_ptrs.size()),
                                  suggested,
                                  dnd_buttonmap_from_keystate(grfKeyState));
      }
      *pdwEffect = accepted ? dnd_action_to_dropeffect(accepted)
                            : DROPEFFECT_NONE;
      return S_OK;
    }

    STDMETHODIMP DragLeave() override
    {
      if (_seam.on_leave) _seam.on_leave(_seam.session_ptr);
      _format_strings.clear();
      _format_ptrs.clear();
      return S_OK;
    }

    STDMETHODIMP Drop(IDataObject* pDataObj, DWORD grfKeyState,
                       POINTL pt, DWORD* pdwEffect) override
    {
      DataItem item;
      dnd_pull_item_from_data_object(pDataObj, item);
      // Refresh formats from the actual data object at drop time.
      cache_formats(pDataObj);

      uint32_t suggested = dnd_dropeffect_suggested(*pdwEffect);
      int x, y;
      screen_to_client(pt, x, y);
      uint32_t accepted = 0;
      if (_seam.on_drop) {
        accepted = _seam.on_drop(_seam.session_ptr, _seam.frame_widget_id,
                                  x, y,
                                  _format_ptrs.data(),
                                  static_cast<uint32_t>(_format_ptrs.size()),
                                  suggested,
                                  dnd_buttonmap_from_keystate(grfKeyState),
                                  &item);
      }
      *pdwEffect = accepted ? dnd_action_to_dropeffect(accepted)
                            : DROPEFFECT_NONE;
      _format_strings.clear();
      _format_ptrs.clear();
      return S_OK;
    }

  private:
    void screen_to_client(POINTL pt, int& out_x, int& out_y)
    {
      POINT p = { pt.x, pt.y };
      ScreenToClient(_hwnd, &p);
      // Convert from physical pixels to logical pixels at 96 DPI.
      UINT dpi = GetDpiForWindow(_hwnd);
      if (dpi == 0) dpi = 96;
      out_x = MulDiv(p.x, 96, static_cast<int>(dpi));
      out_y = MulDiv(p.y, 96, static_cast<int>(dpi));
    }

    void cache_formats(IDataObject* obj)
    {
      _format_strings.clear();
      _format_ptrs.clear();
      if (!obj) return;

      auto note = [&](const std::string& mime) {
        for (auto& existing : _format_strings)
          if (existing == mime) return;
        _format_strings.push_back(mime);
      };

      auto probe = [&](UINT cf, const std::string& mime) {
        FORMATETC fe = { (CLIPFORMAT)cf, nullptr, DVASPECT_CONTENT, -1,
                         TYMED_HGLOBAL };
        if (obj->QueryGetData(&fe) == S_OK) note(mime);
      };
      probe(CF_UNICODETEXT, "text/plain;charset=utf-8");
      if (UINT cf_html = clipboard_cf_html_format())
        probe(cf_html, "text/html");
      probe(CF_HDROP, "text/uri-list");

      IEnumFORMATETC* en = nullptr;
      if (obj->EnumFormatEtc(DATADIR_GET, &en) == S_OK && en) {
        FORMATETC fes[16];
        ULONG fetched = 0;
        while (en->Next(16, fes, &fetched) == S_OK && fetched > 0) {
          for (ULONG i = 0; i < fetched; ++i) {
            UINT cf = fes[i].cfFormat;
            if (cf == CF_UNICODETEXT || cf == CF_TEXT || cf == CF_OEMTEXT ||
                cf == CF_HDROP) continue;
            if (clipboard_cf_html_format() && cf == clipboard_cf_html_format())
              continue;
            char name[256];
            int len = GetClipboardFormatNameA(cf, name, sizeof(name));
            if (len <= 0) continue;
            std::string mime(name, name + len);
            if (mime.find('/') == std::string::npos) continue;
            note(mime);
          }
          fetched = 0;
        }
        en->Release();
      }

      _format_ptrs.reserve(_format_strings.size());
      for (auto& s : _format_strings) _format_ptrs.push_back(s.c_str());
    }

    HWND _hwnd;
    DndDispatchSeam _seam;
    LONG _ref;
    std::vector<std::string>  _format_strings;
    std::vector<const char*>  _format_ptrs;
  };

  // Idempotent global OleInitialize, called once per process by the
  // platform layer before any RegisterDragDrop.
  inline void dnd_ensure_ole_initialized()
  {
    static bool initialized = false;
    if (!initialized) {
      HRESULT hr = OleInitialize(nullptr);
      // S_OK on success; S_FALSE if already initialized (acceptable).
      (void)hr;
      initialized = true;
    }
  }

} // namespace neui_detail

#endif // _WIN32
