#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <neui/neui.h>
#include <cassert>
#include <cstring>
#include <memory>
#include <vector>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <fstream>
#include "window.h"  // provides get_hinstance(), ChildSubclassProc
#include "../../backends/d2d/d2d_backend.h"
#include "../shared/win32/clipboard_win32.h"
#include "../shared/win32/dnd_target_win32.h"
#include "../shared/win32/dnd_source_win32.h"
#include "../shared/win32/icon_win32.h"
#include "../shared/win32/image_loader_win32.h"
#include "../shared/win32/accel_table_win32.h"
#include "../shared/win32/keys_win32.h"
#include "../shared/win32/cursor_win32.h"
#include "../shared/win32/toast_win32.h"
#include "../shared/win32/file_dialog_win32.h"
#include "../shared/shortcut_format.h"
#include "../shared/widget_paint_knob.h"
#include "../shared/widget_paint_section.h"
#include "../shared/widget_paint_tabview.h"
#include "../shared/widget_tabview_host.h"
#include "../shared/widget_paint_grid.h"
#include "../shared/theme_palette.h"
#include "../shared/painter.h"
#include "../shared/widget_paint_compound.h"
#include <commctrl.h>

namespace win32_host
{
  extern std::vector<std::unique_ptr<Session>> sessions;
  bool run();

  // Widget id layout: upper 16 = owning session id, lower 16 = tree slot.
  // The lower-16 slot index is what the Tree<> uses; the upper 16 lets the
  // boundary validate that a handle from session A isn't being applied to
  // session B (relevant for audio plugins where many sessions live in one
  // process). Internal Session methods assume the widget belongs to them
  // and only mask the lower 16; the boundary helpers below enforce that.

  static uint32_t WidgetToIndex(neui_widget_t widget)
  {
    return widget.id & 0xffff;
  }

  // Forward declarations used by helpers higher up in this file.
  void register_frame_as_drop_target_w32(HWND hwnd, Session* s,
                                           uint32_t widget_id);
  static int NEUI_ABI popup_menu(neui_session_t session, neui_widget_t anchor,
                                  int x, int y, const char* const* items);
  // Pack (session_id, slot) into a neui_asset_t handle. Defined later in
  // this TU (alongside the rest of the asset API); forward-declared here
  // because widget_set_text / cascade_dpi / create_child_windows all
  // allocate internal asset slots for the legacy path-source IMAGE branch.
  static neui_asset_t pack_asset_w32(uint32_t session_id, uint32_t slot);

  // GRID painted-widget hooks. Defined at the bottom of this TU; the
  // forward declarations let CreateChildHwnd reference them.
  static void paint_grid_w32(neui_render_backend_t* backend,
                              neui_render_ctx_t      ctx,
                              float w, float h,
                              WidgetData&            wd,
                              bool                   focused);
  static void painted_msg_grid_w32(WidgetData& wd, UINT msg,
                                    WPARAM wParam, LPARAM lParam);

  // SECTION painted-widget hooks. Defined alongside paint_section_w32;
  // forward-declared so CreateChildHwnd can install painted_msg_section_w32
  // when the SECTION opts into scrolling (NEUI_ATTR_SCROLL_MODE != "none").
  static void painted_msg_section_w32(WidgetData& wd, UINT msg,
                                       WPARAM wParam, LPARAM lParam);
  static void section_refresh_scroll_state_w32(WidgetData& wd);
  static void section_reposition_children_w32(WidgetData& sec);
  static void section_external_commit_w32(WidgetData& sec, int nx, int ny);

  // A TABPAGE is a chip-less SECTION: it reuses ALL the section_* machinery
  // (scroll state, inner body HWND, child clipping, kinetics). Every site
  // that early-outs on strcmp(type, NEUI_W_SECTION) instead tests this so
  // pages get the same body/scroll/clip treatment as sections. Mirror of the
  // macOS host's is_section_like.
  static inline bool is_section_like_w32(const char* type)
  {
    return type && (!strcmp(type, NEUI_W_SECTION) ||
                    !strcmp(type, NEUI_W_TABPAGE));
  }
  // The header-chip text the SECTION paint band / window region should use.
  // Empty for a TABPAGE (its `text` is the tab label drawn by the parent
  // TABVIEW, not a section header chip). Mirror of the macOS host's
  // section_effective_text_macos.
  static inline const char* section_effective_text_w32(const WidgetData& wd)
  {
    if (wd.type && !strcmp(wd.type, NEUI_W_TABPAGE)) return "";
    return wd.text.c_str();
  }
  // The chip alignment the SECTION band should use. "none" for a TABPAGE so
  // band_h collapses to 0 + the body fills the whole rect.
  static inline const char* section_effective_align_w32(const WidgetData& wd)
  {
    if (wd.type && !strcmp(wd.type, NEUI_W_TABPAGE)) return "none";
    return wd.attrs ? wd.attrs->get_string(NEUI_ATTR_ALIGN_TEXT) : nullptr;
  }

  // TABVIEW painted-widget hooks + runtime. Defined just before
  // CreateChildHwnd; forward-declared so the section helpers / attr setters /
  // create paths above can reference them. tabview_relayout_w32 is non-static
  // so window.cpp's WM_SIZE handler can call it on a frame resize.
  static void paint_tabview_w32(neui_render_backend_t* backend,
                                 neui_render_ctx_t      ctx,
                                 float w, float h,
                                 WidgetData&            wd,
                                 bool                   focused);
  static void painted_msg_tabview_w32(WidgetData& wd, UINT msg,
                                       WPARAM wParam, LPARAM lParam);
  static void tabview_collect_pages_w32(WidgetData& tv, std::vector<uint32_t>& out);
  static void tabview_apply_page_geometry_w32(WidgetData& tv);
  static void tabview_apply_region_w32(WidgetData& tv);
  static void tabview_select_w32(WidgetData& tv, int new_index);
  void        tabview_relayout_w32(WidgetData& tv);
  // The parent TABVIEW of `wd` if `wd` is one of its TABPAGE children, else
  // nullptr. Used to repaint / re-flow the strip when a page's tab label,
  // chip colours, or background change.
  static inline WidgetData* tabview_parent_of_page_w32(WidgetData& wd)
  {
    if (!wd.session || !wd.type || strcmp(wd.type, NEUI_W_TABPAGE) != 0)
      return nullptr;
    uint32_t pidx = wd.session->_widgets.get_parent(wd.index);
    if (!pidx || !wd.session->_widgets.exists(pidx)) return nullptr;
    auto& pw = wd.session->_widgets[pidx];
    if (pw.type && !strcmp(pw.type, NEUI_W_TABVIEW)) return &pw;
    return nullptr;
  }
  // Timer ID for the scrolling-SECTION spring-back animation on win32
  // native. Per-section: SetTimer is installed on the SECTION's own HWND
  // (unlike xpl where it lives on the frame), so the timer id can stay a
  // small constant.
  static constexpr UINT_PTR SECTION_BOUNCE_TIMER_ID = 0x6E736362;  // 'nscb'

  neui_widget_t IndexToWidget(uint32_t session_id, uint32_t idx)
  {
    return { ((session_id & 0xffff) << 16) | (idx & 0xffff) };
  }

  static Session* get_session(neui_session_t session)
  {
    uint32_t idx = (session.session & 0xffff) - 1;
    if (idx < sessions.size()) {
      return sessions[idx].get();
    }
    return nullptr;
  }

  // True if `widget` is either a sentinel (widget_root / widget_none) or a
  // properly-packed handle whose session id matches `session_id`.
  static bool widget_belongs_to_session(neui_widget_t widget, uint32_t session_id)
  {
    if (widget.id == 0)          return true;   // widget_root
    if (widget.id == UINT32_MAX) return true;   // widget_none
    return ((widget.id >> 16) & 0xffff) == (session_id & 0xffff);
  }

  // Resolve the session for an API call that operates on a specific widget.
  // Returns nullptr if the session is invalid OR the widget belongs to a
  // different session - the call is silently dropped.
  static Session* get_session_for_widget(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session(session);
    if (!s) return nullptr;
    if (!widget_belongs_to_session(widget, s->session_id())) return nullptr;
    return s;
  }

  static int LogicalToPhysical(int logical, UINT dpi)
  {
    return MulDiv(logical, (int)dpi, 96);
  }

  // Widget type -> Win32 class mapping
  struct WidgetTypeInfo {
    const char* neui_type;
    LPCWSTR win32_class;
    DWORD style;
    bool needs_subclass;
  };

  static const WidgetTypeInfo widget_type_table[] = {
    { NEUI_W_LABEL,     L"Static",  WS_CHILD | SS_LEFT,                                                         false },
    { NEUI_W_BUTTON,    L"Button",  WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,                                      true  },
    { NEUI_W_INPUTBOX,  L"Edit",    WS_CHILD | WS_TABSTOP | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL,               true  },
    { NEUI_W_CHECKBOX,  L"Button",  WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,                                    true  },
    { NEUI_W_CHECKBOX3, L"Button",  WS_CHILD | WS_TABSTOP | BS_AUTO3STATE,                                       true  },
    { NEUI_W_LISTBOX,   L"ListBox", WS_CHILD | WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS, true },
    { NEUI_W_COMBOBOX,  L"ComboBox",WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,                  true  },
    { NEUI_W_MULTILINE, L"Edit",    WS_CHILD | WS_TABSTOP | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, true },
    { NEUI_W_TREEVIEW,  L"SysTreeView32",
      WS_CHILD | WS_TABSTOP | WS_BORDER | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS, true },
    { NEUI_W_SLIDER,    TRACKBAR_CLASSW,
      WS_CHILD | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,                                                          true },
  };

  // Message sets for listbox vs combobox, selected by widget type.
  struct ItemsOps {
    UINT add;       // LB_ADDSTRING    / CB_ADDSTRING
    UINT insert;    // LB_INSERTSTRING / CB_INSERTSTRING
    UINT del;       // LB_DELETESTRING / CB_DELETESTRING
    UINT clear;     // LB_RESETCONTENT / CB_RESETCONTENT
    UINT count;     // LB_GETCOUNT     / CB_GETCOUNT
    UINT textlen;   // LB_GETTEXTLEN   / CB_GETLBTEXTLEN
    UINT gettext;   // LB_GETTEXT      / CB_GETLBTEXT
    UINT setdata;   // LB_SETITEMDATA  / CB_SETITEMDATA
    UINT getdata;   // LB_GETITEMDATA  / CB_GETITEMDATA
    UINT getsel;    // LB_GETCURSEL    / CB_GETCURSEL
    UINT setsel;    // LB_SETCURSEL    / CB_SETCURSEL
    UINT desel;     // deselect sentinel value for setsel
  };

  static const ItemsOps listbox_ops  = { LB_ADDSTRING, LB_INSERTSTRING, LB_DELETESTRING, LB_RESETCONTENT, LB_GETCOUNT, LB_GETTEXTLEN,   LB_GETTEXT,   LB_SETITEMDATA, LB_GETITEMDATA, LB_GETCURSEL, LB_SETCURSEL, (UINT)-1 };
  static const ItemsOps combobox_ops = { CB_ADDSTRING, CB_INSERTSTRING, CB_DELETESTRING, CB_RESETCONTENT, CB_GETCOUNT, CB_GETLBTEXTLEN, CB_GETLBTEXT, CB_SETITEMDATA, CB_GETITEMDATA, CB_GETCURSEL, CB_SETCURSEL, (UINT)-1 };

  static const ItemsOps* get_items_ops(const char* type) {
    if (type && !strcmp(type, NEUI_W_LISTBOX))  return &listbox_ops;
    if (type && !strcmp(type, NEUI_W_COMBOBOX)) return &combobox_ops;
    return nullptr;
  }

  // COMBOBOX drop-list sizing. The client lays out only the collapsed bar
  // (the widget x / y / width / height); the drop list is sized independently
  // from the item count - capped at NEUI_ATTR_COMBO_MAX_VISIBLE (default 10) -
  // plus an optional NEUI_ATTR_COMBO_DROP_WIDTH override. We cap the visible
  // rows via CB_SETMINVISIBLE and grow the HWND height so the native dropdown
  // has room; the collapsed control keeps its system height, so the extra
  // height is dropdown-only and invisible while closed (and wd.height /
  // get_size keep reporting the client's logical collapsed height). Re-applied
  // from create_child_windows (covers initial show + DPI flips, where the font
  // is re-broadcast first) and whenever the item set or relevant attrs change.
  static void apply_combo_drop_sizing_w32(WidgetData& wd)
  {
    if (!wd.hwnd || !wd.type || strcmp(wd.type, NEUI_W_COMBOBOX) != 0) return;

    int count  = static_cast<int>(SendMessageW(wd.hwnd, CB_GETCOUNT, 0, 0));
    int maxvis = wd.attrs ? wd.attrs->get_int(NEUI_ATTR_COMBO_MAX_VISIBLE, 10) : 10;
    if (maxvis < 1) maxvis = 1;
    int vis = (count > 0) ? (count < maxvis ? count : maxvis) : 1;

    // Cap the number of rows shown before the list scrolls.
    SendMessageW(wd.hwnd, CB_SETMINVISIBLE, static_cast<WPARAM>(vis), 0);

    // Physical-pixel row geometry (the control font is already DPI-scaled).
    // Item height = CB_GETITEMHEIGHT(0); collapsed-field height = (-1).
    int item_h = static_cast<int>(SendMessageW(wd.hwnd, CB_GETITEMHEIGHT, 0, 0));
    int edit_h = static_cast<int>(SendMessageW(wd.hwnd, CB_GETITEMHEIGHT,
                                               static_cast<WPARAM>(-1), 0));
    if (item_h <= 0) item_h = (edit_h > 0) ? edit_h : 16;
    if (edit_h <= 0) edit_h = item_h;

    RECT rc; GetWindowRect(wd.hwnd, &rc);
    int cur_w  = rc.right - rc.left;
    int drop_h = edit_h + vis * item_h + GetSystemMetrics(SM_CYEDGE) * 4;
    SetWindowPos(wd.hwnd, nullptr, 0, 0, cur_w, drop_h,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    // Drop-list width: explicit override (logical px -> physical), otherwise
    // leave the native default (which tracks the control width).
    int dw = wd.attrs ? wd.attrs->get_int(NEUI_ATTR_COMBO_DROP_WIDTH, 0) : 0;
    if (dw > 0) {
      UINT dpi = GetDpiForWindow(wd.hwnd);
      if (dpi == 0) dpi = 96;
      SendMessageW(wd.hwnd, CB_SETDROPPEDWIDTH,
                   static_cast<WPARAM>(LogicalToPhysical(dw, dpi)), 0);
    }
  }

  static std::wstring ToWide(const char* utf8)
  {
    if (!utf8 || !*utf8) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring result(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &result[0], len);
    return result;
  }

  static std::string FromWide(const wchar_t* wide, int wlen)
  {
    if (!wide || wlen == 0) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, wlen, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string result(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, wlen, &result[0], len, nullptr, nullptr);
    return result;
  }

  static void ApplyText(HWND hwnd, const std::string& text)
  {
    if (hwnd && !text.empty()) {
      SetWindowTextW(hwnd, ToWide(text.c_str()).c_str());
    }
  }

  static HFONT CreateDpiFont(UINT dpi)
  {
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    return CreateFontIndirectW(&ncm.lfMessageFont);
  }

  // Snap CSS-style weight (100..900) to a Win32 LOGFONT::lfWeight value.
  // 0 / unset / out-of-range = FW_NORMAL. Matches the bucketing the d2d
  // backend uses for DWRITE_FONT_WEIGHT_*.
  static int normalise_lfweight_w32(int weight)
  {
    if (weight <= 0)   return FW_NORMAL;
    if (weight < 150)  return FW_THIN;          // 100
    if (weight < 250)  return FW_EXTRALIGHT;    // 200
    if (weight < 350)  return FW_LIGHT;         // 300
    if (weight < 450)  return FW_NORMAL;        // 400
    if (weight < 550)  return FW_MEDIUM;        // 500
    if (weight < 650)  return FW_SEMIBOLD;      // 600
    if (weight < 750)  return FW_BOLD;          // 700
    if (weight < 850)  return FW_EXTRABOLD;     // 800
    return FW_HEAVY;                            // 900+
  }

  // Compose a 64-bit signature for (family, size_q1, weight, dpi) that
  // round-trips through (size_q1, dpi) in the low 32 bits + a 32-bit
  // family hash in the high half. Same signature => no HFONT rebuild.
  static uint64_t font_signature_w32(const char* family, uint32_t size_q1,
                                      int weight, UINT dpi)
  {
    uint32_t fh = 0;
    if (family) {
      for (const char* p = family; *p; ++p)
        fh = fh * 31u + static_cast<uint8_t>(*p);
    }
    fh ^= static_cast<uint32_t>(weight) * 2654435761u;
    uint64_t lo = (static_cast<uint64_t>(dpi) << 16) ^ size_q1;
    return (static_cast<uint64_t>(fh) << 32) | lo;
  }

  // Apply NEUI_ATTR_FONT_FAMILY / NEUI_ATTR_FONT_SIZE / NEUI_ATTR_FONT_WEIGHT
  // to a native control's HFONT. Rebuilds the per-widget HFONT when the
  // (family, size, weight, dpi) tuple differs from the cached signature;
  // when none of the font attrs are set, drops any custom HFONT and falls
  // back to the parent frame's lfMessageFont via WM_SETFONT. Cheap when
  // the signature already matches (one attr lookup + a hash compare).
  static void ensure_custom_font_w32(WidgetData& wd)
  {
    if (!wd.hwnd) return;

    // Read attrs. Unset family + weight + size means "no override" -
    // tear down any prior custom_hfont and reapply the parent's font.
    // Strict typing - the kind assert in AttrBag catches set_int/_float
    // calls against the wrong kind at the call site.
    const char* family = (wd.attrs ? wd.attrs->get_string(NEUI_ATTR_FONT_FAMILY) : nullptr);
    float       size   = (wd.attrs ? wd.attrs->get_float (NEUI_ATTR_FONT_SIZE,   0.0f) : 0.0f);
    int         weight = (wd.attrs ? wd.attrs->get_int   (NEUI_ATTR_FONT_WEIGHT, 0   ) : 0);
    bool fam_set = (family && *family);
    bool sz_set  = (size > 0.0f);
    bool wt_set  = (weight > 0);

    if (!fam_set && !sz_set && !wt_set) {
      if (wd.custom_hfont) {
        // Reapply parent's font so the control doesn't keep our custom one.
        HFONT parent_hfont = wd.session ? wd.session->find_parent_hfont(wd.index) : nullptr;
        SendMessageW(wd.hwnd, WM_SETFONT,
                     reinterpret_cast<WPARAM>(parent_hfont), TRUE);
        DeleteObject(wd.custom_hfont);
        wd.custom_hfont   = nullptr;
        wd.font_signature = 0;
      }
      return;
    }

    UINT dpi = wd.dpi ? wd.dpi : (wd.session ? wd.session->get_dpi_for_widget(wd.index) : 96);
    if (dpi == 0) dpi = 96;

    // Effective size for HFONT: client value (logical px at 96 DPI) when
    // set, else extract the parent frame font's size so we only override
    // family / weight without resizing.
    float eff_size = size;
    if (!sz_set) {
      // Default to the lfMessageFont's logical height at 96 DPI.
      NONCLIENTMETRICSW ncm = {};
      ncm.cbSize = sizeof(ncm);
      SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, 96);
      int h = ncm.lfMessageFont.lfHeight;
      eff_size = static_cast<float>(h < 0 ? -h : (h * 3 / 4));  // pt-ish fallback
      if (eff_size <= 0.0f) eff_size = 12.0f;
    }

    uint32_t size_q1 = static_cast<uint32_t>(eff_size * 10.0f + 0.5f);
    uint64_t sig = font_signature_w32(fam_set ? family : "", size_q1, weight, dpi);
    if (sig == wd.font_signature && wd.custom_hfont) return;

    LOGFONTW lf = {};
    // lfHeight in physical pixels: convert logical-px size -> pt via /72
    // then scale by DPI. Negative = "character height" (matches CreateDpiFont
    // which uses lfMessageFont's negative lfHeight).
    lf.lfHeight    = -MulDiv(static_cast<int>(eff_size + 0.5f), static_cast<int>(dpi), 72);
    lf.lfWeight    = normalise_lfweight_w32(weight);
    lf.lfCharSet   = DEFAULT_CHARSET;
    lf.lfOutPrecision   = OUT_DEFAULT_PRECIS;
    lf.lfClipPrecision  = CLIP_DEFAULT_PRECIS;
    lf.lfQuality        = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    if (fam_set) {
      auto w = ToWide(family);
      wcsncpy_s(lf.lfFaceName, LF_FACESIZE, w.c_str(), _TRUNCATE);
    } else {
      // No family override - clone the system message-font face so the
      // overridden weight/size still uses Segoe UI etc.
      NONCLIENTMETRICSW ncm = {};
      ncm.cbSize = sizeof(ncm);
      SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
      wcsncpy_s(lf.lfFaceName, LF_FACESIZE, ncm.lfMessageFont.lfFaceName, _TRUNCATE);
    }

    HFONT new_hfont = CreateFontIndirectW(&lf);
    if (!new_hfont) return;

    if (wd.custom_hfont) DeleteObject(wd.custom_hfont);
    wd.custom_hfont   = new_hfont;
    wd.font_signature = sig;
    SendMessageW(wd.hwnd, WM_SETFONT,
                 reinterpret_cast<WPARAM>(new_hfont), TRUE);
  }

  // Re-allocate an IMAGE widget's internally-owned asset slot at a new
  // DPI scale so the @2x / @3x variant re-picks. No-op for client-owned
  // assets (the client chose the scale at create_from_file time) and
  // when the resolved variant scale hasn't changed (cheap fast path
  // across DPI flips that don't cross an @Nx boundary).
  static void reload_image_asset_for_dpi_w32(WidgetData& wd, UINT new_dpi)
  {
    if (!wd.image_asset_owned)              return;
    if (wd.image_asset.id == asset_none.id) return;
    if (!wd.session)                        return;
    if (wd.text.empty())                    return;

    float new_scale = static_cast<float>(new_dpi) / 96.0f;
    uint32_t slot = wd.image_asset.id & 0xffff;
    auto* entry = wd.session->_asset_manager.get_slot(slot);
    if (entry && new_scale == entry->scale) return;

    auto* backend = neui_d2d_backend::get_backend();
    wd.session->_asset_manager.release_slot(slot, backend);
    wd.image_asset        = asset_none;
    wd.image_asset_owned  = false;

    uint32_t new_slot = wd.session->_asset_manager.allocate_from_file(
      wd.text, new_scale);
    if (new_slot != 0) {
      wd.image_asset       = pack_asset_w32(wd.session->session_id(), new_slot);
      wd.image_asset_owned = true;
    }
    if (wd.hwnd) InvalidateRect(wd.hwnd, nullptr, FALSE);
  }

  // -------------------------------------------------------------------------
  // Self-painted widget hooks (for the shared "neui.painted" window class).

  static float clamp01_w32(float v)
  {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
  }

  // Snap v to one of N evenly-spaced positions on [0..1]. steps < 2 -> no snap.
  static float snap_to_steps_w32(float v, int steps)
  {
    if (steps < 2) return v;
    int idx = static_cast<int>(v * static_cast<float>(steps - 1) + 0.5f);
    if (idx < 0) idx = 0;
    if (idx >= steps) idx = steps - 1;
    return static_cast<float>(idx) / static_cast<float>(steps - 1);
  }

  static int widget_get_steps_w32(const WidgetData& wd)
  {
    return wd.attrs ? wd.attrs->get_int(NEUI_ATTR_STEPS, 0) : 0;
  }

  // Discrete-or-continuous nudge delta; mirrors xpl host's nudge_delta.
  static float nudge_delta_w32(const WidgetData& wd,
                                int   step_count,
                                float continuous_amount)
  {
    int steps = widget_get_steps_w32(wd);
    if (steps >= 2) {
      return static_cast<float>(step_count) / static_cast<float>(steps - 1);
    }
    return continuous_amount;
  }

  static float widget_get_value_w32(const WidgetData& wd)
  {
    if (!wd.attrs) return 0.0f;
    return clamp01_w32(wd.attrs->get_float(NEUI_PARAM_VALUE, 0.0f));
  }

  // Write value silently (no event); used by drag/wheel after mutating.
  // Fires VALUE_CHANGED iff the value actually changed and emit_events is set.
  static void widget_set_value_user_w32(WidgetData& wd, float v)
  {
    v = snap_to_steps_w32(clamp01_w32(v), widget_get_steps_w32(wd));
    float old = widget_get_value_w32(wd);
    if (v == old) return;
    neui_detail::ensure_attrs(wd.attrs).set_float(NEUI_PARAM_VALUE, v);
    if (wd.emit_events && wd.session) {
      neui_widget_t wid = { wd.widget_id };
      neui_event_t ev{};
      ev.type = NEUI_EVENT_VALUE_CHANGED;
      ev.data.value.widget = wid;
      ev.data.value.value  = v;
      wd.session->dispatch_event(&ev);
    }
    // Push to the native control if applicable. Slider needs TBM_SETPOS to
    // move the thumb; painted widgets just need an InvalidateRect.
    if (wd.hwnd && wd.type && !strcmp(wd.type, NEUI_W_SLIDER)) {
      DWORD style    = static_cast<DWORD>(GetWindowLongW(wd.hwnd, GWL_STYLE));
      bool  vertical = (style & TBS_VERT) != 0;
      int   pos      = static_cast<int>((vertical ? (1.0f - v) : v) * 1000.0f + 0.5f);
      SendMessageW(wd.hwnd, TBM_SETPOS, TRUE, pos);
    } else if (wd.hwnd) {
      InvalidateRect(wd.hwnd, nullptr, FALSE);
    }
  }

  // NEUI_EVENT_GESTURE_BEGIN / _END for the built-in KNOB / SLIDER (their
  // gesture always edits NEUI_PARAM_VALUE). Same gating as VALUE_CHANGED.
  // Non-static so window.cpp's trackbar notification handler can bracket
  // TB_THUMBTRACK..TB_ENDTRACK. Public within the win32_host namespace.
  void widget_emit_gesture_w32(WidgetData& wd, bool begin)
  {
    if (!wd.session || !wd.emit_events) return;
    neui_event_t ev{};
    ev.type = begin ? NEUI_EVENT_GESTURE_BEGIN : NEUI_EVENT_GESTURE_END;
    ev.data.gesture.widget.id = wd.widget_id;
    ev.data.gesture.attr_key  = NEUI_PARAM_VALUE;
    ev.data.gesture.value     = widget_get_value_w32(wd);
    wd.session->dispatch_event(&ev);
  }

  // One-shot user change (wheel tick / key nudge / reset) wrapped in an
  // implicit GESTURE_BEGIN / _END pair. The pair only fires when the snapped
  // value actually moves - checked BEFORE the begin so the VALUE_CHANGED of
  // the write lands between the two. Drags bracket the whole interaction via
  // widget_emit_gesture_w32 at grab / release instead.
  static void widget_set_value_user_gesture_w32(WidgetData& wd, float v)
  {
    float snapped = snap_to_steps_w32(clamp01_w32(v), widget_get_steps_w32(wd));
    if (snapped == widget_get_value_w32(wd)) return;
    widget_emit_gesture_w32(wd, true);
    widget_set_value_user_w32(wd, v);
    widget_emit_gesture_w32(wd, false);
  }

  // Reset a value-bearing widget to NEUI_PARAM_DEFAULT (or 0 if unset).
  // Non-static so window.cpp's ChildSubclassProc can call it for slider
  // double-click. Public within the win32_host namespace. Every caller is a
  // one-shot user action (knob context menu, slider double-click), so the
  // reset carries its own implicit gesture pair.
  void widget_reset_to_default_w32(WidgetData& wd)
  {
    float def = wd.attrs ? wd.attrs->get_float(NEUI_PARAM_DEFAULT, 0.0f) : 0.0f;
    widget_set_value_user_gesture_w32(wd, def);
  }

  // Paint hook used by the "neui.painted" class for KNOB widgets.
  // The shared PaintedWndProc has already cleared the frame to the parent's
  // system background colour, so the bounding rect outside the disc shows
  // that colour - making the knob visually "transparent" to its parent.
  static void paint_knob_w32(neui_render_backend_t* backend,
                              neui_render_ctx_t      ctx,
                              float w, float h,
                              WidgetData&            wd,
                              bool                   focused)
  {
    auto polarity = neui_detail::KNOB_POLARITY_MIN;
    const char* value_text = nullptr;
    if (wd.attrs) {
      polarity   = neui_detail::parse_knob_polarity(wd.attrs->get_string(NEUI_ATTR_POLARITY));
      value_text = wd.attrs->get_string(NEUI_ATTR_VALUE_TEXT);
    }
    int steps = widget_get_steps_w32(wd);
    neui_detail::paint_knob(backend, ctx, 0.0f, 0.0f, w, h,
                             widget_get_value_w32(wd), focused,
                             polarity, steps, value_text, wd.attrs.get());
  }

  // Mouse / wheel hook for KNOB. Implements angular-drag-to-value: the
  // value tracks the cursor's rotation around the knob centre. Per-pixel
  // sensitivity is the natural 1/radius (small circles move the value
  // faster than wide ones). Shift = 1/5 sensitivity, wheel for nudges,
  // double-click = reset to NEUI_PARAM_DEFAULT.
  static constexpr float KNOB_W32_SWEEP_RAD     = 4.71238898f; // 1.5 * PI (270deg)
  static constexpr float KNOB_W32_DEAD_ZONE_R   = 4.0f;        // logical px
  static constexpr float KNOB_W32_FINE_SCALE    = 0.2f;        // Shift = 1/5 sensitivity
  // Slider modes (vertical / horizontal NEUI_ATTR_KNOB_MODE): logical-pixel
  // sweep distance for a full [0..1] traversal.
  static constexpr float KNOB_W32_SLIDER_SWEEP_PX = 200.0f;

  static float wrap_pi_w32(float d)
  {
    const float TWO_PI = 6.28318530717958647692f;
    while (d >  3.14159265358979323846f) d -= TWO_PI;
    while (d < -3.14159265358979323846f) d += TWO_PI;
    return d;
  }

  // Show the knob's right-click context menu and act on the pick.
  // Local coords are in HWND-local logical pixels; popup_menu maps to
  // screen for us.
  static void knob_show_context_menu_w32(WidgetData& wd, int local_x, int local_y)
  {
    if (!wd.session) return;
    static const char* k_items[] = {
      "Reset to default",
      nullptr
    };
    neui_widget_t anchor = { wd.widget_id };
    int picked = popup_menu({ wd.session_id }, anchor, local_x, local_y, k_items);
    if (picked == 1) {
      widget_reset_to_default_w32(wd);
    }
  }

  static void painted_msg_knob_w32(WidgetData& wd, UINT msg,
                                    WPARAM wParam, LPARAM lParam)
  {
    if (msg == WM_MOUSEWHEEL) {
      short delta = static_cast<short>(HIWORD(wParam));
      bool fine = (LOWORD(wParam) & MK_SHIFT) != 0;
      // Wheel up INCREASES knob value, wheel down DECREASES - matching the
      // SLIDER and the natural scroll direction.
      float step = nudge_delta_w32(wd, 1, fine ? 0.01f : 0.05f) *
                   (delta > 0 ? 1.0f : -1.0f);
      widget_set_value_user_gesture_w32(wd, widget_get_value_w32(wd) + step);
      return;
    }

    if (msg == WM_KEYDOWN) {
      // Arrows / Home / End. Snap-aware via nudge_delta_w32. Continuous
      // mode advances by 10% per arrow press (10 presses end-to-end);
      // stepped mode advances by exactly one step. (`small` is a Win32
      // macro for `char`, hence the renamed local.)
      float v    = widget_get_value_w32(wd);
      float step = nudge_delta_w32(wd, 1, 0.10f);
      bool handled = true;
      switch (wParam) {
        case VK_LEFT:
        case VK_DOWN:  v -= step; break;
        case VK_RIGHT:
        case VK_UP:    v += step; break;
        case VK_HOME:  v = 0.0f; break;
        case VK_END:   v = 1.0f; break;
        default:       handled = false; break;
      }
      if (handled)
        widget_set_value_user_gesture_w32(wd, v);
      return;
    }

    UINT dpi = (wd.session ? wd.session->get_dpi_for_widget(wd.index) : 96);
    if (dpi == 0) dpi = 96;

    if (msg == WM_LBUTTONDBLCLK) {
      // CS_DBLCLKS delivered DOWN / UP for the first click, so the first
      // click's drag gesture already closed; the reset is its own pair. If a
      // drag is somehow still in flight, the reset lands inside it (the UP
      // that follows closes it).
      float def = wd.attrs ? clamp01_w32(wd.attrs->get_float(NEUI_PARAM_DEFAULT, 0.0f))
                            : 0.0f;
      if (wd.paint_dragging) widget_set_value_user_w32(wd, def);
      else                   widget_set_value_user_gesture_w32(wd, def);
      return;
    }

    if (msg == WM_RBUTTONDOWN) {
      int lx = MulDiv(GET_X_LPARAM(lParam), 96, static_cast<int>(dpi));
      int ly = MulDiv(GET_Y_LPARAM(lParam), 96, static_cast<int>(dpi));
      knob_show_context_menu_w32(wd, lx, ly);
      return;
    }

    // For DOWN / UP / MOVE we work in HWND-local logical pixels. The knob
    // centre is the centre of the client rect.
    RECT rc{};
    if (wd.hwnd) GetClientRect(wd.hwnd, &rc);
    float cx = static_cast<float>(MulDiv(rc.right,  96, static_cast<int>(dpi))) * 0.5f;
    float cy = static_cast<float>(MulDiv(rc.bottom, 96, static_cast<int>(dpi))) * 0.5f;
    float mx = static_cast<float>(MulDiv(GET_X_LPARAM(lParam), 96, static_cast<int>(dpi)));
    float my = static_cast<float>(MulDiv(GET_Y_LPARAM(lParam), 96, static_cast<int>(dpi)));

    if (msg == WM_LBUTTONDOWN) {
      // Cache the drag mode for the duration of this drag so WM_MOUSEMOVE
      // doesn't pay the attribute-lookup cost. Live changes to the attr
      // take effect on the NEXT drag.
      wd.paint_drag_mode = wd.attrs
        ? wd.attrs->get_int(NEUI_ATTR_KNOB_MODE, NEUI_KNOB_MODE_ROTATIONAL)
        : NEUI_KNOB_MODE_ROTATIONAL;
      int imx = static_cast<int>(mx);
      int imy = static_cast<int>(my);
      if (wd.paint_drag_mode == NEUI_KNOB_MODE_ROTATIONAL) {
        float dx = mx - cx;
        float dy = my - cy;
        if (dx*dx + dy*dy < KNOB_W32_DEAD_ZONE_R * KNOB_W32_DEAD_ZONE_R) {
          // Click bang in the centre: don't start a drag we can't track.
          return;
        }
        wd.paint_drag_prev_angle = std::atan2(dy, dx);
      } else {
        wd.paint_drag_prev_x = imx;
        wd.paint_drag_prev_y = imy;
      }
      wd.paint_dragging = true;
      // Seed the continuous accumulator with the current snapped value so
      // small per-frame deltas accumulate into step crossings rather than
      // being rounded back to the same snapped value each sample.
      wd.paint_drag_continuous = widget_get_value_w32(wd);
      widget_emit_gesture_w32(wd, true);
      SetFocus(wd.hwnd);
      return;
    }
    // WM_CAPTURECHANGED: PaintedWndProc's SetCapture was stolen mid-drag
    // (Alt-Tab, modal popup). Treat it like a release so the gesture closes -
    // a dangling begin wedges host automation.
    if (msg == WM_LBUTTONUP || msg == WM_CAPTURECHANGED) {
      if (wd.paint_dragging) {
        wd.paint_dragging = false;
        widget_emit_gesture_w32(wd, false);
      }
      return;
    }
    if (msg == WM_MOUSEMOVE && wd.paint_dragging) {
      bool  fine     = (wParam & MK_SHIFT) != 0;
      float fine_mul = fine ? KNOB_W32_FINE_SCALE : 1.0f;
      float delta_v  = 0.0f;
      int   imx      = static_cast<int>(mx);
      int   imy      = static_cast<int>(my);
      if (wd.paint_drag_mode == NEUI_KNOB_MODE_VERTICAL) {
        // Up = increase: negative pixel delta -> positive value delta.
        delta_v = -static_cast<float>(imy - wd.paint_drag_prev_y) *
                  (fine_mul / KNOB_W32_SLIDER_SWEEP_PX);
        wd.paint_drag_prev_y = imy;
      } else if (wd.paint_drag_mode == NEUI_KNOB_MODE_HORIZONTAL) {
        delta_v = static_cast<float>(imx - wd.paint_drag_prev_x) *
                  (fine_mul / KNOB_W32_SLIDER_SWEEP_PX);
        wd.paint_drag_prev_x = imx;
      } else {
        float dx = mx - cx;
        float dy = my - cy;
        float r2 = dx*dx + dy*dy;
        if (r2 < KNOB_W32_DEAD_ZONE_R * KNOB_W32_DEAD_ZONE_R) {
          // Inside dead zone - angle is unstable. Drop this sample, but
          // don't end the drag; the user may slip back out.
          return;
        }
        float cur_angle = std::atan2(dy, dx);
        delta_v = wrap_pi_w32(cur_angle - wd.paint_drag_prev_angle) *
                  (fine_mul / KNOB_W32_SWEEP_RAD);
        wd.paint_drag_prev_angle = cur_angle;
      }
      // Accumulate continuously so small per-frame deltas survive across
      // step snapping; only the snapped value is published externally.
      wd.paint_drag_continuous += delta_v;
      wd.paint_drag_continuous = clamp01_w32(wd.paint_drag_continuous);
      widget_set_value_user_w32(wd, wd.paint_drag_continuous);
      return;
    }
  }

  // -------------------------------------------------------------------------
  // CUSTOMDRAW (neui.painted seam) - hands the curated painter API to the
  // client via NEUI_EVENT_WIDGET_PAINT. The transform / clip pushes isolate
  // client state changes so a missing pop doesn't corrupt the next frame.
  // paint_fn runs after PaintedWndProc's begin_frame, so the surface is
  // already cleared to the resolved background colour (NEUI_ATTR_BACKGROUND
  // > theme panel_bg > system COLOR_WINDOW).

  // Resolves a neui_asset_t against the session's W32AssetManager and
  // draws via backend->draw_bitmap. Wired into neui_painter::draw_asset
  // _thunk so the curated painter API can dispatch asset draws without
  // exposing the raw bitmap pointer to clients.
  static void NEUI_ABI w32_painter_draw_asset_thunk(
      void* host_token,
      neui_render_backend_t* backend,
      neui_render_ctx_t ctx,
      neui_asset_t asset,
      float x, float y, float w, float h,
      uint32_t frame,
      uint32_t tint)
  {
    auto* s = static_cast<Session*>(host_token);
    if (!s || !backend || !ctx) return;
    if (asset.id == asset_none.id) return;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return;
    uint32_t slot = asset.id & 0xffff;
    auto* entry = s->_asset_manager.get_slot(slot);
    if (!entry) return;
    // Cache-walk + lazy GPU upload + draw shared with the other hosts
    // (hosts/shared/painter.h). The dispatch helper owns the whole-vs-cell
    // rule (k_draw_asset_whole draws the whole bitmap; a frame index samples
    // one filmstrip cell).
    neui_detail::painter_draw_entry_dispatch(backend, ctx, entry, frame,
                                             x, y, w, h, tint);
  }

  // Invalidate the widget's HWND if it hosts a CUSTOMDRAW compound whose
  // layers depend on state (NEUI_LAYER_STATE_*). Called from PaintedWndProc
  // on hover / press transitions so the compound repaints to swap state-
  // filtered layers in / out. Defined non-static so window.cpp can reach
  // it; forward-declared there.
  void w32_invalidate_if_state_filtered_compound(WidgetData& wd);

  // Resolve a CUSTOMDRAW widget's compound asset to its CompoundAsset
  // storage. Returns nullptr if no compound is attached or the slot has
  // been released. Caller falls back to WIDGET_PAINT in that case.
  static neui_detail::CompoundAsset* resolve_widget_compound_w32(WidgetData& wd)
  {
    if (!wd.session) return nullptr;
    neui_asset_t a = wd.compound_asset;
    if (a.id == asset_none.id) return nullptr;
    if (((a.id >> 16) & 0xffff) != (wd.session->session_id() & 0xffff)) return nullptr;
    auto* e = wd.session->_asset_manager.get_slot(a.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_COMPOUND || !e->compound) return nullptr;
    return e->compound.get();
  }

  void w32_invalidate_if_state_filtered_compound(WidgetData& wd)
  {
    auto* ca = resolve_widget_compound_w32(wd);
    if (!ca) return;
    if (!neui_detail::compound_has_state_filters(*ca)) return;
    if (wd.hwnd) InvalidateRect(wd.hwnd, nullptr, FALSE);
  }

  static void paint_customdraw_w32(neui_render_backend_t* backend,
                                    neui_render_ctx_t      ctx,
                                    float w, float h,
                                    WidgetData&            wd,
                                    bool                   focused)
  {
    if (!wd.session) return;

    if (backend->push_transform) backend->push_transform(ctx);
    if (backend->push_clip)      backend->push_clip(ctx, 0.0f, 0.0f, w, h);

    neui_painter painter{};
    painter.backend          = backend;
    painter.ctx              = ctx;
    painter.host_token       = wd.session;
    painter.draw_asset_thunk = &w32_painter_draw_asset_thunk;

    if (auto* ca = resolve_widget_compound_w32(wd)) {
      // Compound mode: paint z<0 then z>=0 back-to-back. Child HWNDs
      // paint independently above the parent surface, so the conceptual
      // z=0 child slot is irreducibly fixed at "after the parent paint" -
      // both the below and above layers land before child HWNDs render.
      const neui_detail::AttrBag* bag = neui_detail::attrs_readonly(wd.attrs);
      bool selected = neui_detail::attr_as_float(bag, NEUI_ATTR_SELECTED, 0.0f) != 0.0f;
      uint32_t state_mask = neui_detail::compose_widget_state(
                              wd.enabled, wd.hovered, wd.pressed, selected);
      neui_detail::paint_compound_below(&painter, *ca, w, h, bag, state_mask);
      neui_detail::paint_compound_above(&painter, *ca, w, h, bag, state_mask);
    } else {
      neui_event_t ev{};
      ev.type = NEUI_EVENT_WIDGET_PAINT;
      ev.data.paint.widget.id   = wd.widget_id;
      ev.data.paint.painter_api = &neui_detail::k_painter_api;
      ev.data.paint.p           = &painter;
      ev.data.paint.width       = w;
      ev.data.paint.height      = h;
      ev.data.paint.focused     = focused;
      // No user zoom on the native host (child widgets are real HWNDs sized by
      // the OS), so the device scale is purely the monitor's DPI ratio.
      ev.data.paint.scale       =
        (backend && backend->get_scale_factor) ? backend->get_scale_factor(ctx) : 1.0f;
      wd.session->dispatch_event(&ev);
    }

    if (backend->pop_clip)      backend->pop_clip(ctx);
    if (backend->pop_transform) backend->pop_transform(ctx);
  }

  // Resolve a CUSTOMDRAW widget's behavior asset to its BehaviorAsset.
  // Returns nullptr if no behavior is attached or the slot was released.
  static neui_detail::BehaviorAsset* resolve_widget_behavior_w32(WidgetData& wd)
  {
    if (!wd.session) return nullptr;
    neui_asset_t a = wd.behavior_asset;
    if (a.id == asset_none.id) return nullptr;
    if (((a.id >> 16) & 0xffff) != (wd.session->session_id() & 0xffff)) return nullptr;
    auto* e = wd.session->_asset_manager.get_slot(a.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_BEHAVIOR || !e->behavior) return nullptr;
    return e->behavior.get();
  }

  // Host callbacks for behavior dispatch on win32 native.
  static void w32_behavior_invalidate(void* host_data)
  {
    auto* wd = static_cast<WidgetData*>(host_data);
    if (wd && wd->hwnd) InvalidateRect(wd->hwnd, nullptr, FALSE);
  }

  static void w32_behavior_emit_attr_changed(void* host_data,
                                               const char* attr_key, float value)
  {
    auto* wd = static_cast<WidgetData*>(host_data);
    if (!wd || !wd->session || !wd->emit_events) return;
    neui_event_t ev{};
    ev.type                 = NEUI_EVENT_ATTR_CHANGED;
    ev.data.attr.widget.id  = wd->widget_id;
    ev.data.attr.attr_key   = attr_key;
    ev.data.attr.value      = value;
    wd->session->dispatch_event(&ev);
  }

  static void w32_behavior_emit_gesture(void* host_data,
                                          const char* attr_key, float value,
                                          bool begin)
  {
    auto* wd = static_cast<WidgetData*>(host_data);
    if (!wd || !wd->session || !wd->emit_events) return;
    neui_event_t ev{};
    ev.type = begin ? NEUI_EVENT_GESTURE_BEGIN : NEUI_EVENT_GESTURE_END;
    ev.data.gesture.widget.id = wd->widget_id;
    ev.data.gesture.attr_key  = attr_key;
    ev.data.gesture.value     = value;
    wd->session->dispatch_event(&ev);
  }

  static int w32_behavior_popup_menu(void* host_data, int local_x, int local_y,
                                       const char* const* items)
  {
    auto* wd = static_cast<WidgetData*>(host_data);
    if (!wd || !items) return 0;
    neui_widget_t anchor = { wd->widget_id };
    return popup_menu({ wd->session_id }, anchor, local_x, local_y, items);
  }

  // Forward-decl: defined later alongside the rest of the DnD API thunks.
  static neui_dnd_action_t NEUI_ABI dnd_begin_drag_with_preview(
                                                    neui_session_t,
                                                    neui_widget_t,
                                                    neui_data_item_t,
                                                    uint32_t,
                                                    const neui_drag_preview_t*);

  static uint32_t w32_behavior_begin_drag(void* host_data,
                                            neui_data_item_t item,
                                            uint32_t allowed_actions,
                                            uint32_t preview_image,
                                            int hot_x, int hot_y)
  {
    auto* wd = static_cast<WidgetData*>(host_data);
    if (!wd) return NEUI_DND_ACTION_NONE;
    neui_session_t sess = { wd->session_id };
    neui_widget_t  wid  = { wd->widget_id };
    neui_drag_preview_t preview = { { preview_image }, hot_x, hot_y };
    return static_cast<uint32_t>(
      dnd_begin_drag_with_preview(sess, wid, item, allowed_actions,
                                    preview_image ? &preview : nullptr));
  }

  static neui_detail::BehaviorDispatchCtx make_behavior_ctx_w32(WidgetData& wd)
  {
    neui_detail::BehaviorDispatchCtx ctx{};
    ctx.bag      = &neui_detail::ensure_attrs(wd.attrs);
    ctx.widget_w = static_cast<float>(wd.width);
    ctx.widget_h = static_cast<float>(wd.height);
    ctx.host_data         = &wd;
    ctx.invalidate        = &w32_behavior_invalidate;
    ctx.emit_attr_changed = &w32_behavior_emit_attr_changed;
    ctx.emit_gesture      = &w32_behavior_emit_gesture;
    ctx.popup_menu        = &w32_behavior_popup_menu;
    ctx.begin_drag        = &w32_behavior_begin_drag;
    return ctx;
  }

  // Translate WM_* params into a neui_event_t suitable for the shared
  // behavior dispatch. Returns false if msg is not a supported mouse event.
  static bool w32_build_mouse_event(WidgetData& wd, UINT msg,
                                      WPARAM wParam, LPARAM lParam,
                                      neui_event_t& out_ev,
                                      float& out_lx, float& out_ly)
  {
    UINT dpi = (wd.session ? wd.session->get_dpi_for_widget(wd.index) : 96);
    if (dpi == 0) dpi = 96;
    int lx = MulDiv(GET_X_LPARAM(lParam), 96, static_cast<int>(dpi));
    int ly = MulDiv(GET_Y_LPARAM(lParam), 96, static_cast<int>(dpi));
    out_lx = static_cast<float>(lx);
    out_ly = static_cast<float>(ly);
    neui_widget_t wid = { wd.widget_id };
    switch (msg) {
      case WM_LBUTTONDOWN:
        out_ev = { NEUI_EVENT_MOUSE_BUTTON_DOWN };
        out_ev.data.mouse = { wid, lx, ly, neui_detail::win32_buttonmap(wParam) };
        return true;
      case WM_LBUTTONUP:
        out_ev = { NEUI_EVENT_MOUSE_BUTTON_UP };
        out_ev.data.mouse = { wid, lx, ly, neui_detail::win32_buttonmap(wParam) };
        return true;
      case WM_MOUSEMOVE:
        out_ev = { NEUI_EVENT_MOUSE_MOVE };
        out_ev.data.mouse = { wid, lx, ly, neui_detail::win32_buttonmap(wParam) };
        return true;
      case WM_RBUTTONDOWN:
        out_ev = { NEUI_EVENT_MOUSE_RBUTTON_DOWN };
        out_ev.data.mouse = { wid, lx, ly, neui_detail::win32_buttonmap(wParam) };
        return true;
      case WM_RBUTTONUP:
        out_ev = { NEUI_EVENT_MOUSE_RBUTTON_UP };
        out_ev.data.mouse = { wid, lx, ly, neui_detail::win32_buttonmap(wParam) };
        return true;
      case WM_MOUSEWHEEL: {
        short delta = static_cast<short>(HIWORD(wParam));
        out_ev = { NEUI_EVENT_MOUSE_WHEEL };
        out_ev.data.wheel.widget = wid;
        out_ev.data.wheel.x      = lx;
        out_ev.data.wheel.y      = ly;
        out_ev.data.wheel.delta  = delta / WHEEL_DELTA;
        // On the wheel messages wParam packs the delta in the HIGH word and
        // the key/button state in the LOW word, so unlike the mouse cases
        // above it needs GET_KEYSTATE_WPARAM rather than the whole wParam.
        out_ev.data.wheel.buttonmap =
          neui_detail::win32_buttonmap(GET_KEYSTATE_WPARAM(wParam));
        return true;
      }
      default:
        return false;
    }
  }

  // Mouse / key hook for CUSTOMDRAW. Sequence:
  //   1. ChildSubclassProc already fired NEUI_EVENT_MOUSE_* / KEY* to the
  //      client (emit_events is auto-set; ignores client return).
  //   2. This runs next. If a behavior asset is attached, dispatch through
  //      hosts/shared/behavior_runtime.h, which writes attrs and emits
  //      NEUI_EVENT_ATTR_CHANGED for user-driven mutations.
  //   3. The default WM_* handling (focus on click) runs after.
  static void painted_msg_customdraw_w32(WidgetData& wd, UINT msg,
                                          WPARAM wParam, LPARAM lParam)
  {
    if (msg == WM_LBUTTONDOWN && wd.hwnd) {
      SetFocus(wd.hwnd);
    }

    auto* ba = resolve_widget_behavior_w32(wd);
    if (!ba) return;
    if (!wd.behavior_rt)
      wd.behavior_rt = std::make_unique<neui_detail::BehaviorRuntime>();
    auto ctx = make_behavior_ctx_w32(wd);

    if (msg == WM_CAPTURECHANGED) {
      // Capture stolen mid-drag: synthesize the release so an in-flight
      // behavior drag ends (and closes its gesture) instead of dangling.
      // Arrives synchronously from PaintedWndProc's own ReleaseCapture too;
      // the rt.dragging guard makes that a no-op.
      if (wd.behavior_rt->dragging) {
        neui_event_t up{};
        up.type                 = NEUI_EVENT_MOUSE_BUTTON_UP;
        up.data.mouse.widget.id = wd.widget_id;
        neui_detail::behavior_dispatch_mouse(*ba, *wd.behavior_rt, ctx,
                                              &up, 0.0f, 0.0f);
      }
      return;
    }

    if (msg == WM_KEYDOWN) {
      // Modifiers: read live (the WM_KEYDOWN message doesn't carry them).
      uint32_t mods = neui_detail::win32_kmod_from_state();
      neui_detail::behavior_dispatch_key(*ba, *wd.behavior_rt, ctx,
                                          static_cast<uint32_t>(wParam), mods);
      return;
    }

    neui_event_t ev;
    float local_x = 0.0f, local_y = 0.0f;
    if (!w32_build_mouse_event(wd, msg, wParam, lParam, ev, local_x, local_y))
      return;
    neui_detail::behavior_dispatch_mouse(*ba, *wd.behavior_rt, ctx,
                                          &ev, local_x, local_y);
  }

  // -------------------------------------------------------------------------
  // IMAGE (neui.painted seam) - non-interactive aspect-fitted bitmap.
  // Source is the widget's image_asset; both legacy path-source
  // (allocated internally by widget_set_text) and client-supplied
  // handles flow through here. PaintedWndProc has already cleared the
  // surface to the resolved background (NEUI_ATTR_BACKGROUND > theme
  // panel_bg > system COLOR_WINDOW), so we just lazy-upload the GPU
  // bitmap and draw it.

  static void paint_image_w32(neui_render_backend_t* backend,
                               neui_render_ctx_t      ctx,
                               float w, float h,
                               WidgetData&            wd,
                               bool                   /*focused*/)
  {
    if (wd.image_asset.id == asset_none.id) return;
    if (!wd.session) return;

    // Outer transform + clip mirror paint_customdraw_w32 so client-set
    // rotation can't draw outside the widget rect.
    if (backend->push_transform) backend->push_transform(ctx);
    if (backend->push_clip)      backend->push_clip(ctx, 0.0f, 0.0f, w, h);

    uint32_t asset_sess = (wd.image_asset.id >> 16) & 0xffff;
    if (asset_sess != (wd.session->session_id() & 0xffff)) {
      if (backend->pop_clip)      backend->pop_clip(ctx);
      if (backend->pop_transform) backend->pop_transform(ctx);
      return;
    }

    uint32_t slot = wd.image_asset.id & 0xffff;
    auto* entry = wd.session->_asset_manager.get_slot(slot);
    if (!entry) {
      if (backend->pop_clip)      backend->pop_clip(ctx);
      if (backend->pop_transform) backend->pop_transform(ctx);
      return;
    }

    // Lazy GPU upload per (asset, ctx) pair with device-loss check
    // (mirrors w32_painter_draw_asset_thunk).
    const uint32_t gen = backend->get_context_generation
      ? backend->get_context_generation(ctx) : 0u;
    auto it = entry->bitmaps.find(ctx);
    if (it != entry->bitmaps.end() && it->second.generation != gen) {
      if (backend->destroy_bitmap && it->second.bmp)
        backend->destroy_bitmap(ctx, it->second.bmp);
      entry->bitmaps.erase(it);
      it = entry->bitmaps.end();
    }
    if (it == entry->bitmaps.end()) {
      if (!backend->create_bitmap) {
        if (backend->pop_clip)      backend->pop_clip(ctx);
        if (backend->pop_transform) backend->pop_transform(ctx);
        return;
      }
      void* bmp = backend->create_bitmap(ctx,
                                          entry->width_px, entry->height_px,
                                          entry->pixels.data(),
                                          entry->scale);
      if (!bmp) {
        if (backend->pop_clip)      backend->pop_clip(ctx);
        if (backend->pop_transform) backend->pop_transform(ctx);
        return;
      }
      it = entry->bitmaps.emplace(ctx, W32CtxBitmap{ bmp, gen }).first;
    }

    // Aspect-preserving fit: scale the source to touch one of the widget
    // bounds and centre on the other axis (letterbox / pillarbox).
    float dst_x = 0.0f, dst_y = 0.0f, dst_w = w, dst_h = h;
    float img_w_log = (entry->scale > 0.0f)
      ? static_cast<float>(entry->width_px)  / entry->scale : 0.0f;
    float img_h_log = (entry->scale > 0.0f)
      ? static_cast<float>(entry->height_px) / entry->scale : 0.0f;
    if (img_w_log > 0.0f && img_h_log > 0.0f && w > 0.0f && h > 0.0f) {
      float widget_aspect = w / h;
      float image_aspect  = img_w_log / img_h_log;
      if (image_aspect > widget_aspect) {
        dst_w = w;
        dst_h = w / image_aspect;
        dst_x = 0.0f;
        dst_y = (h - dst_h) * 0.5f;
      } else {
        dst_h = h;
        dst_w = h * image_aspect;
        dst_y = 0.0f;
        dst_x = (w - dst_w) * 0.5f;
      }
    }

    // Optional rotation around the destination centre (the aspect-fitted
    // rect, so rotation doesn't wobble when the image is letterboxed).
    float rot = (wd.attrs ? wd.attrs->get_float(NEUI_ATTR_ROTATION, 0.0f) : 0.0f);
    bool rotated = (rot != 0.0f) && backend->push_transform != nullptr;
    if (rotated) {
      float cx = dst_x + dst_w * 0.5f;
      float cy = dst_y + dst_h * 0.5f;
      backend->push_transform(ctx);
      backend->translate(ctx,  cx,  cy);
      backend->rotate   (ctx, rot);
      backend->translate(ctx, -cx, -cy);
    }

    if (backend->draw_bitmap)
      backend->draw_bitmap(ctx, it->second.bmp,
                            0.0f, 0.0f, 0.0f, 0.0f,
                            dst_x, dst_y, dst_w, dst_h,
                            0xFFFFFFFFu);

    if (rotated)                  backend->pop_transform(ctx);
    if (backend->pop_clip)        backend->pop_clip(ctx);
    if (backend->pop_transform)   backend->pop_transform(ctx);
  }

  // -------------------------------------------------------------------------
  // SECTION (neui.painted seam) - non-interactive coloured backdrop with
  // an optional title chip. paint_section_w32 delegates to the shared
  // helper so the visual matches the xpl host. apply_section_region_w32
  // installs a window region (body rect + chip rect, in physical px) so
  // the title band's non-chip area is truly transparent and the parent
  // frame's pixels show through underneath.

  // Refresh wd.section_scroll_state from NEUI_ATTR_SCROLL_MODE. Allocates
  // when the section opts into a scroll axis, drops when it opts out. Safe
  // to call repeatedly. Matches the xpl host's SectionWidget::refresh_scroll_state.
  static void section_refresh_scroll_state_w32(WidgetData& wd)
  {
    if (!is_section_like_w32(wd.type)) return;
    const char* mode = wd.attrs ? wd.attrs->get_string(NEUI_ATTR_SCROLL_MODE) : nullptr;
    auto axis = neui_detail::parse_section_scroll_mode(mode);
    if (axis == neui_detail::SectionScrollAxis::None) {
      wd.section_scroll_state.reset();
      return;
    }
    if (!wd.section_scroll_state)
      wd.section_scroll_state =
        std::make_unique<neui_detail::SectionScrollState>();
    wd.section_scroll_state->axis = axis;
  }

  // Compute the SECTION's body / scrollbar layout for the current size +
  // attrs + content extent. Cached on wd.section_last_layout so the
  // painted-msg hook (drag, hit-test, kinetics) can read it back without
  // re-measuring. Updates wd.section_scroll_state->content_w/h.
  static void section_compute_layout_w32(WidgetData& wd)
  {
    if (!wd.session) return;
    const char* align = section_effective_align_w32(wd);
    int band_h = neui_detail::section_band_h_for(section_effective_text_w32(wd),
                                                  wd.height, align);
    int initial_body_w = wd.width;
    int initial_body_h = wd.height - band_h;
    if (initial_body_h < 0) initial_body_h = 0;

    int content_w = 0, content_h = 0;
    auto autofn = [&](int& w, int& h){
      neui_detail::section_compute_auto_extent(wd.session->_widgets,
                                                 wd.index, w, h);
    };
    neui_detail::resolve_section_content_extent(wd.attrs.get(), autofn,
                                                  initial_body_w, initial_body_h,
                                                  content_w, content_h);

    auto axis = wd.section_scroll_state
                  ? wd.section_scroll_state->axis
                  : neui_detail::SectionScrollAxis::None;
    wd.section_last_layout = neui_detail::compute_section_layout(
                               wd.width, wd.height, band_h,
                               content_w, content_h, axis);
    if (wd.section_scroll_state) {
      wd.section_scroll_state->content_w = content_w;
      wd.section_scroll_state->content_h = content_h;
      // Idle clamp: keep mid-gesture rubber-band overshoot, snap any
      // externally-caused out-of-range (resize / content shrunk) back in.
      neui_detail::clamp_section_scroll_idle(*wd.section_scroll_state,
                                               content_w, content_h,
                                               wd.section_last_layout.body_w,
                                               wd.section_last_layout.body_h);
    }
  }

  // If `widget_index`'s parent is a scrolling SECTION, return its scroll
  // offset in logical px (the value subtracted from the child's stored
  // body-local (x, y) to derive its body_hwnd-local HWND coords). For
  // non-scrolling sections both outputs stay 0 - children parent to the
  // section's own HWND at section-local coords, preserving today's
  // behaviour.
  static void parent_scroll_offset_w32(Session* sess, uint32_t widget_index,
                                        int& out_off_x, int& out_off_y)
  {
    out_off_x = 0; out_off_y = 0;
    if (!sess) return;
    uint32_t parent_idx = sess->_widgets.get_parent(widget_index);
    if (parent_idx == 0 || !sess->_widgets.exists(parent_idx)) return;
    auto& pw = sess->_widgets[parent_idx];
    if (pw.section_scroll_state) {
      out_off_x = pw.section_scroll_state->scroll_x;
      out_off_y = pw.section_scroll_state->scroll_y;
    }
  }

  // Return the HWND that should be used as the win32 parent for child
  // widgets of `parent_wd`. For a scrolling SECTION this is the inner
  // body HWND; for everything else it's the widget's own HWND. The body
  // HWND only exists when the section is scrolling so children of
  // non-scrolling sections fall through to the legacy section-HWND
  // parenting unchanged.
  static HWND section_child_parent_hwnd_w32(const WidgetData& parent_wd)
  {
    if (parent_wd.section_body_hwnd) return parent_wd.section_body_hwnd;
    return parent_wd.hwnd;
  }

  // Create the section's inner body HWND. Called from CreateChildHwnd
  // for a scrolling section, and from the SCROLL_MODE attr setter when
  // the section transitions from non-scrolling to scrolling at runtime.
  // body_hwnd is positioned at the section's body rect (in section-client
  // physical px) so children parented to it lay out body-local. Stores
  // the section's WidgetData* in body_hwnd's GWLP_USERDATA so the wndproc
  // can read attrs for paint.
  //
  // WS_EX_COMPOSITED enables off-screen double-buffering for the body +
  // all its descendants: the OS composites the body fill and every child
  // into a backbuffer, then blits the result in one pass. This is the
  // standard Win32 idiom for flicker-free scrolling containers - it
  // eliminates the visible-intermediate-state during the brief window
  // between SetWindowPos moving a child and the child's WM_PAINT firing.
  // (Docs nominally restrict the flag to top-level windows, but in
  // practice the compositor honours it on child HWNDs too, and the
  // alternative - hand-rolled memory-DC offscreen compositing - has the
  // same effect at much higher complexity.)
  static void section_create_body_hwnd_w32(WidgetData& sec)
  {
    if (!sec.hwnd || sec.section_body_hwnd) return;
    // Compute the body rect for the current geometry; positions below
    // get clamped on every layout change anyway.
    section_compute_layout_w32(sec);
    UINT dpi = sec.session ? sec.session->get_dpi_for_widget(sec.index) : 96;
    if (dpi == 0) dpi = 96;
    const auto& L = sec.section_last_layout;
    int phys_x = LogicalToPhysical(L.body_x, dpi);
    int phys_y = LogicalToPhysical(L.body_y, dpi);
    int phys_w = LogicalToPhysical(L.body_w > 0 ? L.body_w : 1, dpi);
    int phys_h = LogicalToPhysical(L.body_h > 0 ? L.body_h : 1, dpi);
    HWND body = CreateWindowExW(WS_EX_COMPOSITED, L"neui.sectionbody", L"",
                                 WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN
                                   | WS_CLIPSIBLINGS,
                                 phys_x, phys_y, phys_w, phys_h,
                                 sec.hwnd, nullptr, get_hinstance(),
                                 &sec);
    sec.section_body_hwnd = body;
  }

  // (The section's body HWND is now kept for the section's lifetime once
  // created - see the SCROLL_MODE setter - and torn down only by the
  // DestroyWindow cascade in widget_destroy, so there is no standalone
  // destroy helper.)

  // Re-parent every direct child of `sec` between section.hwnd and
  // body_hwnd depending on `to_body`. Called when SCROLL_MODE flips at
  // runtime so existing children move to the correct container. The
  // child's stored (x, y) is body-local for scrolling sections and the
  // contract is preserved by the per-host SetWindowPos that follows.
  static void section_reparent_children_w32(WidgetData& sec, bool to_body)
  {
    if (!sec.session) return;
    HWND new_parent = to_body ? sec.section_body_hwnd : sec.hwnd;
    if (!new_parent) return;
    uint32_t idx = sec.session->_widgets.child(sec.index);
    while (idx != 0) {
      if (sec.session->_widgets.exists(idx)) {
        auto& cw = sec.session->_widgets[idx];
        if (cw.hwnd && GetParent(cw.hwnd) != new_parent)
          SetParent(cw.hwnd, new_parent);
      }
      idx = sec.session->_widgets.next(idx);
    }
  }

  // Reposition every direct child HWND of a SECTION to its scroll-adjusted
  // coordinates. For a scrolling section, children are parented to
  // body_hwnd and positioned at body_hwnd-local `(wd.x - scroll_x,
  // wd.y - scroll_y)`. Win32's default child-window clipping confines
  // them to body_hwnd's client rect, so no per-child SetWindowRgn is
  // needed and the OS's standard paint pipeline handles repaints (the
  // window manager invalidates exposed strips on its own when child
  // positions move). For non-scrolling sections this is a no-op shape
  // (sx, sy = 0) that just re-applies the existing section-local
  // coords - safe to call after any geometry change.
  static void section_reposition_children_w32(WidgetData& sec)
  {
    if (!sec.hwnd || !sec.session) return;
    UINT dpi = sec.session->get_dpi_for_widget(sec.index);
    if (dpi == 0) dpi = 96;
    int sx = sec.section_scroll_state ? sec.section_scroll_state->scroll_x : 0;
    int sy = sec.section_scroll_state ? sec.section_scroll_state->scroll_y : 0;
    HDWP hdwp = BeginDeferWindowPos(8);
    uint32_t idx = sec.session->_widgets.child(sec.index);
    while (idx != 0) {
      if (sec.session->_widgets.exists(idx)) {
        auto& cw = sec.session->_widgets[idx];
        if (cw.hwnd) {
          int phys_x = LogicalToPhysical(cw.x - sx, dpi);
          int phys_y = LogicalToPhysical(cw.y - sy, dpi);
          int phys_w = LogicalToPhysical(cw.width,  dpi);
          int phys_h = LogicalToPhysical(cw.height, dpi);
          UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
          if (hdwp) {
            hdwp = DeferWindowPos(hdwp, cw.hwnd, nullptr,
                                   phys_x, phys_y, phys_w, phys_h, flags);
          } else {
            SetWindowPos(cw.hwnd, nullptr, phys_x, phys_y, phys_w, phys_h,
                          flags);
          }
        }
      }
      idx = sec.session->_widgets.next(idx);
    }
    if (hdwp) EndDeferWindowPos(hdwp);
  }

  // After any change that can affect a SECTION's layout (resize,
  // scroll-mode attr, content extent attr, chip align attr), recompute
  // the layout cache, resize the inner body_hwnd to match the new body
  // rect, re-clamp the scroll position, and reposition every child.
  // No-op when the HWND hasn't been created yet (deferred creation) -
  // the same recompute will run on first paint anyway. Non-static so
  // window.cpp's WM_SIZE handler can call it.
  //
  // body_hwnd is explicitly invalidated regardless of whether its rect
  // changed. WS_EX_COMPOSITED puts body_hwnd's paint behind an off-screen
  // composition buffer; on a resize the OS auto-invalidates the newly-
  // covered area, but a same-size change (chip align flip from "left" to
  // "right" keeps band_h identical) doesn't queue any body_hwnd redraw,
  // and the section's own InvalidateRect below is masked by
  // WS_CLIPCHILDREN over body_hwnd's area. Without the explicit
  // invalidate the body's pixels go stale - the chip moves but the body
  // underneath keeps its last frame.
  void section_apply_layout_changes_w32(WidgetData& wd)
  {
    if (!wd.hwnd || !is_section_like_w32(wd.type)) return;
    section_compute_layout_w32(wd);
    if (wd.section_body_hwnd) {
      UINT dpi = wd.session ? wd.session->get_dpi_for_widget(wd.index) : 96;
      if (dpi == 0) dpi = 96;
      const auto& L = wd.section_last_layout;
      int phys_x = LogicalToPhysical(L.body_x, dpi);
      int phys_y = LogicalToPhysical(L.body_y, dpi);
      int phys_w = LogicalToPhysical(L.body_w > 0 ? L.body_w : 1, dpi);
      int phys_h = LogicalToPhysical(L.body_h > 0 ? L.body_h : 1, dpi);
      SetWindowPos(wd.section_body_hwnd, nullptr,
                    phys_x, phys_y, phys_w, phys_h,
                    SWP_NOZORDER | SWP_NOACTIVATE);
      InvalidateRect(wd.section_body_hwnd, nullptr, FALSE);
    }
    section_reposition_children_w32(wd);
    InvalidateRect(wd.hwnd, nullptr, FALSE);
  }

  static void paint_section_w32(neui_render_backend_t* backend,
                                 neui_render_ctx_t      ctx,
                                 float w, float h,
                                 WidgetData&            wd,
                                 bool                   /*focused*/)
  {
    using neui_detail::ColorRole;
    uint32_t bg = neui_detail::shade(
                    neui_detail::color(ColorRole::frame_bg),
                    neui_detail::SECTION_BG_LIFT);
    if (wd.attrs && wd.attrs->has(NEUI_ATTR_BACKGROUND))
      bg = static_cast<uint32_t>(wd.attrs->get_int(NEUI_ATTR_BACKGROUND, 0));
    // A TABPAGE is a chip-less section: section_effective_*_w32 return
    // ""/"none" so paint_section fills the whole rect with no header band.
    const char* align = section_effective_align_w32(wd);
    neui_detail::paint_section(backend, ctx, 0.0f, 0.0f, w, h,
                                section_effective_text_w32(wd), bg, align,
                                neui_detail::color(ColorRole::text_primary),
                                wd.attrs.get());

    // Late-refresh in case set_string(SCROLL_MODE) ran without the explicit
    // refresh hook firing - cheap when state already matches. Mirrors the
    // xpl host's SectionWidget::paint safety net.
    section_refresh_scroll_state_w32(wd);

    section_compute_layout_w32(wd);

    if (wd.section_scroll_state &&
        (wd.section_last_layout.vert_sb_shown ||
         wd.section_last_layout.horz_sb_shown)) {
      uint32_t sep   = neui_detail::color(ColorRole::scrollbar_separator);
      uint32_t track = neui_detail::color(ColorRole::scrollbar_track);
      uint32_t thumb = neui_detail::color(ColorRole::scrollbar_thumb);
      neui_detail::paint_section_scrollbars(backend, ctx,
                                              wd.section_last_layout,
                                              *wd.section_scroll_state,
                                              sep, track, thumb);
    }
  }

  // Fire NEUI_EVENT_SCROLL_CHANGED if the section's committed offset has
  // moved since the last notification. SectionScrollState tracks
  // last_notified_x/y (INT32_MIN sentinel for "never notified"). Used by
  // every commit path: wheel kinetic + stepped, scrollbar drag, bounce
  // tick, programmatic scroll API.
  static void section_notify_scroll_changed_w32(WidgetData& wd)
  {
    if (!wd.session || !wd.section_scroll_state) return;
    auto& st = *wd.section_scroll_state;
    if (st.scroll_x == st.last_notified_x &&
        st.scroll_y == st.last_notified_y) return;
    st.last_notified_x = st.scroll_x;
    st.last_notified_y = st.scroll_y;
    neui_event_t ev{};
    ev.type                  = NEUI_EVENT_SCROLL_CHANGED;
    ev.data.scroll.widget.id = wd.widget_id;
    ev.data.scroll.scroll_x  = st.scroll_x;
    ev.data.scroll.scroll_y  = st.scroll_y;
    wd.session->dispatch_event(&ev);
  }

  // Feed a synthetic precise wheel delta into a section's per-axis
  // kinetics. dv / dh are logical px with the kinetics' sign convention
  // (positive = scroll up / left); the function picks the right axis,
  // commits the new scroll position, repositions children, and starts
  // the spring-back timer on overscroll release. No-op when the section
  // isn't scrolling. Mirror of the xpl host's section_kinetic_wheel_w32.
  static void section_kinetic_wheel_native_w32(WidgetData& wd,
                                                 double dv, double dh)
  {
    using namespace neui_detail;
    if (!wd.section_scroll_state || !wd.hwnd) return;
    auto& st = *wd.section_scroll_state;
    auto& L  = wd.section_last_layout;
    bool has_v = section_axis_has_v(st.axis);
    bool has_h = section_axis_has_h(st.axis);
    // Asymmetric single-axis fallback (same rule as xpl): a horizontal-only
    // section absorbs a pure vertical wheel because classic wheel mice have
    // no horizontal axis; a vertical-only section does NOT absorb pure
    // horizontal input (explicit horizontal intent should be ignored).
    if (!has_v && has_h && dh == 0.0 && dv != 0.0) { dh = dv; dv = 0.0; }

    int  kin_mode = section_read_kinetics_mode(wd.attrs.get());
    bool smooth   = scroll_kinetics_smooth_enabled(kin_mode,
                                                    /*platform_default_smooth=*/false);

    bool changed = false;
    bool start_bounce = false;
    if (smooth) {
      ScrollWheelAction act_v{}, act_h{};
      if (has_v && dv != 0.0) {
        ScrollWheelInput in;
        in.precise  = true;
        in.delta_px = dv;
        act_v = section_scroll_wheel_kinetic(st, L, in, false);
      }
      if (has_h && dh != 0.0) {
        ScrollWheelInput in;
        in.precise  = true;
        in.delta_px = dh;
        act_h = section_scroll_wheel_kinetic(st, L, in, true);
      }
      changed      = act_v.changed      || act_h.changed;
      start_bounce = act_v.start_bounce || act_h.start_bounce;
    } else {
      // STEPPED: hard-clamp, no rubber-band, no spring-back.
      if (has_v && dv != 0.0 && section_scroll_step_px(st, L, dv, false))
        changed = true;
      if (has_h && dh != 0.0 && section_scroll_step_px(st, L, dh, true))
        changed = true;
    }
    if (changed) {
      section_reposition_children_w32(wd);
      InvalidateRect(wd.hwnd, nullptr, FALSE);
      section_notify_scroll_changed_w32(wd);
    }
    if (start_bounce)
      SetTimer(wd.hwnd, SECTION_BOUNCE_TIMER_ID, 16, nullptr);
  }

  // Message hook for scrolling SECTION. PaintedWndProc forwards mouse /
  // wheel / timer / destroy events through wd.painted_msg_fn; the hook
  // owns scroll-state mutations + child repositioning. Non-scrolling
  // sections never reach here (painted_msg_fn is left null at create).
  static void painted_msg_section_w32(WidgetData& wd, UINT msg,
                                       WPARAM wParam, LPARAM lParam)
  {
    using namespace neui_detail;
    if (!wd.section_scroll_state || !wd.hwnd) return;
    auto& st = *wd.section_scroll_state;
    auto& L  = wd.section_last_layout;

    // Teardown: cancel any running bounce timer so a HWND-keyed slot stays
    // clean across slot reuse.
    if (msg == WM_DESTROY) {
      KillTimer(wd.hwnd, SECTION_BOUNCE_TIMER_ID);
      return;
    }

    // Spring-back tick.
    if (msg == WM_TIMER && wParam == SECTION_BOUNCE_TIMER_ID) {
      bool more_v = section_scroll_bounce_step(st, L, false);
      bool more_h = section_scroll_bounce_step(st, L, true);
      section_reposition_children_w32(wd);
      InvalidateRect(wd.hwnd, nullptr, FALSE);
      section_notify_scroll_changed_w32(wd);
      if (!more_v && !more_h)
        KillTimer(wd.hwnd, SECTION_BOUNCE_TIMER_ID);
      return;
    }

    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
      short delta_raw = (short)HIWORD(wParam);
      UINT lines = 3;
      SystemParametersInfo(msg == WM_MOUSEWHEEL ? SPI_GETWHEELSCROLLLINES
                                                 : SPI_GETWHEELSCROLLCHARS,
                            0, &lines, 0);
      if (lines == 0) lines = 3;
      double notches = (double)delta_raw / (double)WHEEL_DELTA;
      double delta_px = notches * (double)lines * SECTION_WHEEL_LINE_PX;
      // Shift+wheel routes the vertical wheel to the horizontal axis -
      // the classic Win32 "horizontal scroll without a tilt wheel" trick.
      // WM_MOUSEHWHEEL: positive raw delta = tilt right. The kinetics
      // sign convention is "positive delta_px = scroll left" (raw_px -=
      // delta), so negate for the horizontal path - matches the xpl
      // platform_win32 SECTION wheel handler.
      bool shift_held = (wParam & MK_SHIFT) != 0;
      if (msg == WM_MOUSEWHEEL && !shift_held)
        section_kinetic_wheel_native_w32(wd, delta_px, 0.0);
      else
        section_kinetic_wheel_native_w32(wd, 0.0, -delta_px);
      return;
    }

    if (msg == WM_LBUTTONDOWN || msg == WM_MOUSEMOVE || msg == WM_LBUTTONUP) {
      UINT dpi = wd.session ? wd.session->get_dpi_for_widget(wd.index) : 96;
      if (dpi == 0) dpi = 96;
      // lParam carries client-area physical px; convert to widget-local
      // logical px.
      int phys_x = GET_X_LPARAM(lParam);
      int phys_y = GET_Y_LPARAM(lParam);
      int local_x = MulDiv(phys_x, 96, dpi);
      int local_y = MulDiv(phys_y, 96, dpi);

      if (msg == WM_LBUTTONDOWN) {
        int hit = section_scrollbar_hit(L, local_x, local_y);
        if (hit == 1) {
          st.vert_drag.active           = true;
          st.vert_drag.start_axis_coord = local_y;
          st.vert_drag.start_position   = st.scroll_y;
          // PaintedWndProc already called SetCapture(hwnd), so we'll
          // receive WM_MOUSEMOVE / WM_LBUTTONUP regardless of where the
          // cursor goes.
        } else if (hit == 2) {
          st.horz_drag.active           = true;
          st.horz_drag.start_axis_coord = local_x;
          st.horz_drag.start_position   = st.scroll_x;
        }
        return;
      }

      if (msg == WM_MOUSEMOVE) {
        if (st.vert_drag.active) {
          auto geom = compute_scrollbar(L.body_h, 0, st.content_h,
                                          L.body_h, st.vert_drag.start_position);
          int new_y = scrollbar_drag_apply(st.vert_drag, local_y, geom,
                                             st.content_h, L.body_h);
          if (new_y != st.scroll_y) {
            st.scroll_y = new_y;
            // External mutation: drop any kinetic overshoot claim on this
            // axis so the next idle clamp pulls it back in if needed.
            st.kinetic_over_v = false;
            section_reposition_children_w32(wd);
            InvalidateRect(wd.hwnd, nullptr, FALSE);
            section_notify_scroll_changed_w32(wd);
          }
        } else if (st.horz_drag.active) {
          auto geom = compute_scrollbar(L.body_w, 0, st.content_w,
                                          L.body_w, st.horz_drag.start_position);
          int new_x = scrollbar_drag_apply(st.horz_drag, local_x, geom,
                                             st.content_w, L.body_w);
          if (new_x != st.scroll_x) {
            st.scroll_x = new_x;
            st.kinetic_over_h = false;
            section_reposition_children_w32(wd);
            InvalidateRect(wd.hwnd, nullptr, FALSE);
            section_notify_scroll_changed_w32(wd);
          }
        }
        return;
      }

      if (msg == WM_LBUTTONUP) {
        st.vert_drag.active = false;
        st.horz_drag.active = false;
        return;
      }
    }
  }

  // Build and apply the section's window region so anything outside
  // (body U title chip) is clipped away. Called from PaintedWndProc on
  // create + size, and from set_text / NEUI_ATTR_ALIGN_TEXT live updates.
  // Logical (96 DPI) measurements are converted to physical px for the
  // GDI region. If `text` is empty the region is the full rect (no band).
  // Non-static so PaintedWndProc in window.cpp can call it from WM_SIZE.
  void apply_section_region_w32(WidgetData& wd)
  {
    if (!wd.hwnd) return;
    UINT dpi = wd.session ? wd.session->get_dpi_for_widget(wd.index) : 96;
    if (dpi == 0) dpi = 96;
    int phys_w = LogicalToPhysical(wd.width,  dpi);
    int phys_h = LogicalToPhysical(wd.height, dpi);
    if (phys_w <= 0 || phys_h <= 0) {
      SetWindowRgn(wd.hwnd, nullptr, FALSE);
      return;
    }

    const char* align       = section_effective_align_w32(wd);
    const char* region_text = section_effective_text_w32(wd);
    if (!region_text[0] || neui_detail::section_align_is_none(align)) {
      // No band reserved - full rect (a TABPAGE always lands here).
      SetWindowRgn(wd.hwnd, CreateRectRgn(0, 0, phys_w, phys_h), FALSE);
      return;
    }

    auto* backend = neui_d2d_backend::get_backend();
    float tw = (backend && backend->measure_text)
                 ? backend->measure_text(nullptr, region_text, -1,
                                          neui_detail::SECTION_LABEL_FONT)
                 : 0.0f;
    auto chip = neui_detail::section_chip_rect(
                  0.0f,
                  static_cast<float>(wd.width),
                  static_cast<float>(wd.height),
                  tw, align);

    // Round to nearest physical pixel; chip width is clamped to widget
    // width by the helper so chip_right cannot overflow phys_w.
    auto to_phys = [dpi](float logical) {
      return LogicalToPhysical(static_cast<int>(logical + 0.5f), dpi);
    };
    int band_h_phys = to_phys(chip.band_h);
    int chip_l = to_phys(chip.chip_x);
    int chip_r = to_phys(chip.chip_x + chip.chip_w);
    if (band_h_phys > phys_h) band_h_phys = phys_h;
    if (chip_l < 0)           chip_l      = 0;
    if (chip_r > phys_w)      chip_r      = phys_w;

    HRGN body = CreateRectRgn(0, band_h_phys, phys_w, phys_h);
    HRGN chip_rgn = CreateRectRgn(chip_l, 0, chip_r, band_h_phys);
    CombineRgn(body, body, chip_rgn, RGN_OR);
    DeleteObject(chip_rgn);
    // SetWindowRgn takes ownership of `body`; do not delete it ourselves.
    // bRedraw=FALSE so we don't loop when called from inside WM_PAINT.
    SetWindowRgn(wd.hwnd, body, FALSE);

    // The region may have SHRUNK relative to its previous shape (e.g.
    // the chip just moved from "left" to "center", so the strip the chip
    // used to occupy at the left of the band is now outside the region).
    // Windows does not auto-repaint the parent in pixels a child window
    // just stopped covering, so the old chip's frame_bg "ghost" lingers
    // there. Invalidate the parent in the section's full bounding rect;
    // WS_CLIPCHILDREN on the frame clips the parent's paint to the area
    // OUTSIDE the section's (new) region, so only the newly-exposed
    // pixels actually repaint.
    if (HWND parent = GetParent(wd.hwnd)) {
      RECT sec_rect;
      GetWindowRect(wd.hwnd, &sec_rect);
      MapWindowPoints(HWND_DESKTOP, parent,
                      reinterpret_cast<POINT*>(&sec_rect), 2);
      InvalidateRect(parent, &sec_rect, TRUE);
    }
  }

  // -------------------------------------------------------------------------
  // TABVIEW runtime. The geometry math lives in hosts/shared/widget_tabview.h;
  // here the win32 host enumerates the TABPAGE children, lays out the chip
  // strip (label widths measured via the d2d backend's null-ctx path, exactly
  // like apply_section_region_w32), sizes the selected page's HWND to the
  // content body rect, toggles page visibility, and fires the deselect/select
  // events. Mirror of the xpl host's TabViewWidget + the macOS host's
  // tabview_*_macos helpers.

  // Collect the TABVIEW's NEUI_W_TABPAGE child indices in creation (tab) order.
  static void tabview_collect_pages_w32(WidgetData& tv, std::vector<uint32_t>& out)
  {
    neui_detail::tabview_collect_pages(tv.session, tv.index, out);
  }

  // Re-measure labels, resolve the strip edge / size, lay out the chips into
  // wd.tab_chips, and cache the content body rect in wd.section_last_layout.
  // Pure state - no GDI, no child HWND moves - so it is safe to call from
  // paint as well as the relayout triggers. Returns the full TabViewLayout
  // (the shared paint helper needs the strip rect too, which the cached
  // SectionLayout does not carry).
  static neui_detail::TabViewLayout tabview_recompute_layout_w32(WidgetData& wd)
  {
    using namespace neui_detail;
    TabViewLayout L{};
    if (!wd.session) return L;

    const char* pos = wd.attrs ? wd.attrs->get_string(NEUI_ATTR_TAB_POSITION) : nullptr;
    TabPosition tp = parse_tab_position(pos);
    wd.tab_edge = tp.edge;

    std::vector<uint32_t> pages;
    tabview_collect_pages_w32(wd, pages);
    int count = static_cast<int>(pages.size());
    if (wd.tab_selected >= count) wd.tab_selected = count > 0 ? count - 1 : 0;
    if (wd.tab_selected < 0)      wd.tab_selected = 0;

    EffectiveFont ef = read_widget_font(wd.attrs.get(), TAB_CHIP_FONT);
    auto* backend = neui_d2d_backend::get_backend();
    // Measure chip labels only when the label set or font changed - measure_text
    // is comparatively expensive and this runs on every paint as well as on
    // relayout. Cached widths are keyed by the shared tab_labels_signature.
    std::vector<const char*> labels(count, "");
    for (int i = 0; i < count; ++i)
      labels[i] = wd.session->_widgets[pages[i]].text.c_str();
    uint64_t sig = tab_labels_signature(labels.data(), count,
                       ef.family.c_str(), ef.weight, ef.size);
    if (sig != wd.tab_label_sig || static_cast<int>(wd.tab_label_widths.size()) != count) {
      wd.tab_label_widths.assign(count, 0.0f);
      if (backend && backend->measure_text)
        for (int i = 0; i < count; ++i)
          wd.tab_label_widths[i] = backend->measure_text(nullptr, labels[i], -1, ef.size);
      wd.tab_label_sig = sig;
    }
    const float* widths = wd.tab_label_widths.data();

    float explicit_strip = wd.attrs
                  ? static_cast<float>(wd.attrs->get_int(NEUI_ATTR_TAB_STRIP_SIZE, 0)) : 0.0f;
    float strip = tab_resolve_strip_size(tp.edge, explicit_strip, widths, count);
    L = compute_tabview_layout(static_cast<float>(wd.width),
                                static_cast<float>(wd.height), tp.edge, strip);

    wd.tab_chips.assign(count, TabChip{});
    if (count > 0 && tp.edge != TabEdge::None)
      layout_tab_chips(L, tp.edge, tp.align, widths, count, wd.tab_chips.data());

    wd.section_last_layout = SectionLayout{};
    wd.section_last_layout.body_x = static_cast<int>(L.body_x);
    wd.section_last_layout.body_y = static_cast<int>(L.body_y);
    wd.section_last_layout.body_w = static_cast<int>(L.body_w);
    wd.section_last_layout.body_h = static_cast<int>(L.body_h);
    return L;
  }

  // Per-side inset (logical px) the opaque page HWND is shrunk by inside the
  // content body, so the lines the TABVIEW paints at the body perimeter stay
  // visible around the page: the strip<->body baseline (always drawn) on the
  // strip side, and the optional content border (NEUI_ATTR_TAB_BORDER_COLOR)
  // on the three far sides. (xpl / macOS paint the page in the same surface,
  // so they don't need this; the win32 page is a separate opaque child HWND
  // that would otherwise cover those lines.)
  static void tabview_page_insets_w32(WidgetData& tv, int& top, int& left,
                                       int& bottom, int& right)
  {
    bool has_border = tv.attrs && tv.attrs->has(NEUI_ATTR_TAB_BORDER_COLOR) &&
                      tv.attrs->get_int(NEUI_ATTR_TAB_BORDER_COLOR, 0) != 0;
    float bw = tv.attrs ? static_cast<float>(tv.attrs->get_int(NEUI_ATTR_TAB_BORDER_WIDTH, 0)) : 0.0f;
    neui_detail::tabview_page_insets(tv.tab_edge, has_border, bw, top, left, bottom, right);
  }

  // Size each TABPAGE HWND to the cached content body rect (minus the border
  // insets), show the selected one + hide the rest, and re-flow each page's
  // own body / children. Uses the body rect from the most recent recompute
  // (section_last_layout).
  static void tabview_apply_page_geometry_w32(WidgetData& tv)
  {
    if (!tv.session) return;
    std::vector<uint32_t> pages;
    tabview_collect_pages_w32(tv, pages);
    int count = static_cast<int>(pages.size());
    if (count == 0) return;
    if (tv.tab_selected < 0)      tv.tab_selected = 0;
    if (tv.tab_selected >= count) tv.tab_selected = count - 1;

    const auto& L = tv.section_last_layout;
    int it = 0, il = 0, ib = 0, ir = 0;
    tabview_page_insets_w32(tv, it, il, ib, ir);
    int px = L.body_x + il;
    int py = L.body_y + it;
    int pw_ = L.body_w - il - ir; if (pw_ < 0) pw_ = 0;
    int ph_ = L.body_h - it - ib; if (ph_ < 0) ph_ = 0;

    UINT dpi = tv.session->get_dpi_for_widget(tv.index);
    if (dpi == 0) dpi = 96;
    for (int i = 0; i < count; ++i) {
      auto& pg = tv.session->_widgets[pages[i]];
      bool active = (i == tv.tab_selected);
      pg.x = px; pg.y = py;
      pg.width  = pw_;
      pg.height = ph_;
      pg.visible = active;
      if (pg.hwnd) {
        // Active page sits at the content body rect, topmost among the
        // sibling pages, shown; inactive pages go to the bottom + hidden.
        SetWindowPos(pg.hwnd, active ? HWND_TOP : HWND_BOTTOM,
          LogicalToPhysical(pg.x, dpi), LogicalToPhysical(pg.y, dpi),
          LogicalToPhysical(pg.width  > 0 ? pg.width  : 1, dpi),
          LogicalToPhysical(pg.height > 0 ? pg.height : 1, dpi),
          SWP_NOACTIVATE);
        ShowWindow(pg.hwnd, active ? SW_SHOWNA : SW_HIDE);
        // Re-flow the page's own body view + children to the new size (it is
        // a chip-less section). SetWindowPos fires WM_SIZE only when the size
        // changed; call explicitly so same-size repositions still re-flow.
        section_apply_layout_changes_w32(pg);
      }
    }

    // Erase the just-hidden page's leftover pixels. Hiding a page only
    // invalidates that page's own (now-hidden) ancestor chain, so the child
    // HWNDs we just hid (labels, and especially a scrolling page's
    // WS_EX_COMPOSITED body HWND + its rows) leave their last-rendered pixels
    // in the top-level window's shared redirection surface at the old body
    // position. Invalidating the tabview or the active page alone does NOT
    // reliably overwrite them (the only thing that did was a window resize).
    // So replicate a resize's repaint: ask the ROOT window to synchronously
    // redraw the tabview's screen rect with RDW_ALLCHILDREN, which repaints
    // every window intersecting that rect (tabview + the now-visible page +
    // its children) bottom-to-top, laying fresh opaque pixels over the stale
    // ones. Not re-entrant - painting never calls back into selection.
    if (tv.hwnd) {
      HWND root = GetAncestor(tv.hwnd, GA_ROOT);
      RECT tr; GetWindowRect(tv.hwnd, &tr);
      if (root) {
        MapWindowPoints(HWND_DESKTOP, root, reinterpret_cast<POINT*>(&tr), 2);
        RedrawWindow(root, &tr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
      } else {
        RedrawWindow(tv.hwnd, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
      }
    }
  }

  // Set the TABVIEW's window region to (content body rect U each chip rect),
  // in physical px, so the strip gutter beside the chips is truly transparent
  // (the parent frame shows through) - exactly the technique
  // apply_section_region_w32 uses for the SECTION title band. Recomputed from
  // the cached layout + chips on every relayout (NOT from paint, so the
  // parent-invalidate below can't storm). For TabEdge::None the region is the
  // full rect.
  static void tabview_apply_region_w32(WidgetData& tv)
  {
    if (!tv.hwnd) return;
    UINT dpi = tv.session ? tv.session->get_dpi_for_widget(tv.index) : 96;
    if (dpi == 0) dpi = 96;
    int phys_w = LogicalToPhysical(tv.width, dpi);
    int phys_h = LogicalToPhysical(tv.height, dpi);
    if (phys_w <= 0 || phys_h <= 0) { SetWindowRgn(tv.hwnd, nullptr, FALSE); return; }

    if (tv.tab_edge == neui_detail::TabEdge::None || tv.tab_chips.empty()) {
      SetWindowRgn(tv.hwnd, CreateRectRgn(0, 0, phys_w, phys_h), FALSE);
      return;
    }

    const auto& L = tv.section_last_layout;
    auto clampw = [&](int v){ return v < 0 ? 0 : (v > phys_w ? phys_w : v); };
    auto clamph = [&](int v){ return v < 0 ? 0 : (v > phys_h ? phys_h : v); };
    // The baseline + content-box edge that paint_tabview strokes sit ON the
    // body's strip-facing boundary, spanning the FULL strip width (skipping
    // only the active chip). Where a chip covers that span the line is inside
    // the region, but in the empty gutter beside the chips it lands just
    // outside the body rect and gets clipped (the missing line left of a
    // right-aligned active chip). Grow the body rect by the line width toward
    // the strip so that boundary line is always included.
    float bw = tv.attrs ? static_cast<float>(tv.attrs->get_int(NEUI_ATTR_TAB_BORDER_WIDTH, 0)) : 0.0f;
    int line = bw > 0.0f ? static_cast<int>(bw + 0.5f) : 1;
    if (line < 1) line = 1;
    int bx0 = L.body_x, by0 = L.body_y;
    int bx1 = L.body_x + L.body_w, by1 = L.body_y + L.body_h;
    switch (tv.tab_edge) {
      case neui_detail::TabEdge::Top:    by0 -= line; break; // strip above body
      case neui_detail::TabEdge::Bottom: by1 += line; break; // strip below body
      case neui_detail::TabEdge::Left:   bx0 -= line; break;
      case neui_detail::TabEdge::Right:  bx1 += line; break;
      default: break;
    }
    HRGN rgn = CreateRectRgn(clampw(LogicalToPhysical(bx0, dpi)),
                             clamph(LogicalToPhysical(by0, dpi)),
                             clampw(LogicalToPhysical(bx1, dpi)),
                             clamph(LogicalToPhysical(by1, dpi)));
    for (const auto& c : tv.tab_chips) {
      int cx0 = clampw(LogicalToPhysical(static_cast<int>(c.x + 0.5f), dpi));
      int cy0 = clamph(LogicalToPhysical(static_cast<int>(c.y + 0.5f), dpi));
      int cx1 = clampw(LogicalToPhysical(static_cast<int>(c.x + c.w + 0.5f), dpi));
      int cy1 = clamph(LogicalToPhysical(static_cast<int>(c.y + c.h + 0.5f), dpi));
      HRGN cr = CreateRectRgn(cx0, cy0, cx1, cy1);
      CombineRgn(rgn, rgn, cr, RGN_OR);
      DeleteObject(cr);
    }
    SetWindowRgn(tv.hwnd, rgn, FALSE);  // takes ownership of rgn

    // The region may have shrunk (e.g. position flipped from a wide top strip
    // to a narrow side strip); the parent doesn't auto-repaint pixels the
    // child just stopped covering. Invalidate the parent across the widget's
    // full bounds - WS_CLIPCHILDREN clips the parent's paint to the area
    // outside the new region, so only newly-exposed pixels repaint.
    if (HWND parent = GetParent(tv.hwnd)) {
      RECT wr; GetWindowRect(tv.hwnd, &wr);
      MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(&wr), 2);
      InvalidateRect(parent, &wr, TRUE);
    }
  }

  // Switch the active tab. If the selection actually changes, fire
  // NEUI_EVENT_TAB_DESELECTED (old) then _SELECTED (new) BEFORE swapping page
  // visibility + repainting, so a client handler can update the incoming
  // page's widgets first. Mirror of xpl TabViewWidget::select_tab.
  static void tabview_select_w32(WidgetData& tv, int ni)
  {
    // Clamp + fire TAB_DESELECTED / TAB_SELECTED + re-resolve the selection in
    // the shared helper; only swap page geometry + repaint when it changed.
    if (neui_detail::tabview_commit_selection(tv.session, tv.widget_id, tv.index,
                                              tv.tab_selected, ni)) {
      tabview_apply_page_geometry_w32(tv);
      if (tv.hwnd) InvalidateRect(tv.hwnd, nullptr, FALSE);
    }
  }

  // Full re-flow: recompute the strip layout, re-size / re-show the pages,
  // repaint the strip. Called from create_child_windows (after the pages
  // exist), the frame WM_SIZE handler, DPI cascade, the TABVIEW / TABPAGE
  // attr setters, set_text on a page, and page add / destroy.
  void tabview_relayout_w32(WidgetData& tv)
  {
    if (!tv.session || !tv.hwnd) return;
    tabview_recompute_layout_w32(tv);
    // Set the new window region BEFORE repainting. A position change moves
    // the body rect (and the region) a lot; apply_page_geometry ends with a
    // synchronous full repaint, so the region must already be the new one or
    // the active page paints under the stale region + lands outside it (the
    // page came up empty after a position change).
    tabview_apply_region_w32(tv);
    tabview_apply_page_geometry_w32(tv);
  }

  static void paint_tabview_w32(neui_render_backend_t* backend,
                                 neui_render_ctx_t      ctx,
                                 float w, float h,
                                 WidgetData&            wd,
                                 bool                   /*focused*/)
  {
    using namespace neui_detail;
    if (!wd.session) return;

    TabViewLayout L = tabview_recompute_layout_w32(wd);

    std::vector<uint32_t> pages;
    tabview_collect_pages_w32(wd, pages);
    int count = static_cast<int>(pages.size());

    std::vector<const char*> labels(count, "");
    std::vector<uint32_t>    chip_bg(count, 0), chip_text(count, 0);
    for (int i = 0; i < count; ++i) {
      auto& pw = wd.session->_widgets[pages[i]];
      labels[i] = pw.text.c_str();
      if (pw.attrs) {
        chip_bg[i]   = static_cast<uint32_t>(pw.attrs->get_int(NEUI_ATTR_TAB_CHIP_BG_COLOR, 0));
        chip_text[i] = static_cast<uint32_t>(pw.attrs->get_int(NEUI_ATTR_TAB_CHIP_TEXT_COLOR, 0));
      }
    }

    // Resolve the tabview chrome colours (shared with xpl / macOS). The active
    // page's NEUI_ATTR_BACKGROUND drives body_bg so the active chip reads as
    // connected to its page.
    const AttrBag* active_attrs =
      (count > 0) ? wd.session->_widgets[pages[wd.tab_selected]].attrs.get() : nullptr;
    TabPaintColors tc = resolve_tab_paint_colors(wd.attrs.get(), active_attrs);

    // The strip gutter beside the chips is left unpainted by paint_tabview;
    // the TABVIEW's window region (tabview_apply_region_w32) clips the HWND
    // to (body U chips) so that gutter is transparent and the parent frame
    // shows through, matching xpl / macOS + the SECTION title band.
    paint_tabview(backend, ctx, 0.0f, 0.0f, w, h, L, wd.tab_edge,
                   wd.tab_chips.data(), count, wd.tab_selected,
                   labels.data(), chip_bg.data(), chip_text.data(),
                   tc.body_bg, tc.default_text, tc.inactive_chip_bg,
                   tc.sep_color, tc.border_w, tc.strip_bg, tc.content_border,
                   tc.chip_radius, wd.attrs.get());
  }

  static void painted_msg_tabview_w32(WidgetData& wd, UINT msg,
                                       WPARAM wParam, LPARAM lParam)
  {
    if (!wd.enabled) return;
    if (msg == WM_LBUTTONDOWN) {
      UINT dpi = wd.session ? wd.session->get_dpi_for_widget(wd.index) : 96;
      if (dpi == 0) dpi = 96;
      float lx = static_cast<float>(GET_X_LPARAM(lParam)) * 96.0f / static_cast<float>(dpi);
      float ly = static_cast<float>(GET_Y_LPARAM(lParam)) * 96.0f / static_cast<float>(dpi);
      int hit = neui_detail::tabview_chip_hit(wd.tab_chips.data(),
                                              static_cast<int>(wd.tab_chips.size()), lx, ly);
      if (hit >= 0) {
        if (wd.hwnd) SetFocus(wd.hwnd);   // so the arrow keys reach this widget
        tabview_select_w32(wd, hit);
      }
      return;
    }
    if (msg == WM_KEYDOWN) {
      if (wParam == VK_LEFT || wParam == VK_UP)
        tabview_select_w32(wd, wd.tab_selected - 1);
      else if (wParam == VK_RIGHT || wParam == VK_DOWN)
        tabview_select_w32(wd, wd.tab_selected + 1);
      return;
    }
  }

  // -------------------------------------------------------------------------

  static HWND CreateChildHwnd(WidgetData& wd, HWND parent_hwnd, UINT parent_dpi)
  {
    // Control IDs (the HMENU arg to CreateWindowExW below) must stay in
    // the lower half of the 16-bit WM_COMMAND space so they don't alias
    // menu cmd_ids (which live in [0x8000, 0xFFFF] - see WidgetData's
    // next_menu_cmd_id). 32K live widgets per session is unreachable in
    // practice; this is a debug-only safety net.
    assert(wd.index < 0x8000 &&
           "widget tree-slot index would alias the menu cmd_id range");

    // Image widget: shared "neui.painted" class; paint_fn does aspect-fit
    // + rotation + draw_bitmap. Non-interactive (no subclass, no
    // painted_msg_fn, emit_events stays false), so it matches the SECTION
    // shape rather than KNOB / CUSTOMDRAW.
    if (!strcmp(wd.type, NEUI_W_IMAGE)) {
      DWORD style = WS_CHILD;
      if (wd.visible) style |= WS_VISIBLE;
      wd.paint_fn = &paint_image_w32;
      HWND hwnd = CreateWindowExW(0, L"neui.painted", L"", style,
        LogicalToPhysical(wd.x, parent_dpi),
        LogicalToPhysical(wd.y, parent_dpi),
        LogicalToPhysical(wd.width, parent_dpi),
        LogicalToPhysical(wd.height, parent_dpi),
        parent_hwnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(wd.index)),
        get_hinstance(),
        &wd);
      if (hwnd) wd.hwnd = hwnd;
      return hwnd;
    }

    // Knob (and future painted controls) use the shared "neui.painted" class.
    // Paint logic and mouse hooks plug in via WidgetData function pointers.
    if (!strcmp(wd.type, NEUI_W_KNOB)) {
      DWORD style = WS_CHILD | WS_TABSTOP;
      if (wd.visible) style |= WS_VISIBLE;
      wd.paint_fn       = &paint_knob_w32;
      wd.painted_msg_fn = &painted_msg_knob_w32;
      HWND hwnd = CreateWindowExW(0, L"neui.painted", L"", style,
        LogicalToPhysical(wd.x, parent_dpi),
        LogicalToPhysical(wd.y, parent_dpi),
        LogicalToPhysical(wd.width, parent_dpi),
        LogicalToPhysical(wd.height, parent_dpi),
        parent_hwnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(wd.index)),
        get_hinstance(),
        &wd);
      if (hwnd) {
        SetWindowSubclass(hwnd, ChildSubclassProc, 1, reinterpret_cast<DWORD_PTR>(&wd));
        wd.has_subclass = true;
      }
      return hwnd;
    }

    // Client custom-draw widget: shared painted-class HWND, paint_fn emits
    // WIDGET_PAINT to the client, ChildSubclassProc emits standard mouse /
    // key events (emit_events auto-set in widget_create above).
    if (!strcmp(wd.type, NEUI_W_CUSTOMDRAW)) {
      DWORD style = WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS;
      if (wd.visible) style |= WS_VISIBLE;
      // NEUI_ATTR_INPUT_TRANSPARENT: WS_EX_TRANSPARENT makes the child pass
      // mouse hits through to the window beneath it in z-order, mirroring the
      // crossplatform host's "decorative, never hit-tested" contract. The
      // widget still paints normally (full opacity).
      // NEUI_ATTR_OVERLAY: WS_EX_NOREDIRECTIONBITMAP drops the opaque
      // redirection surface so the backend renders the widget through a
      // DirectComposition visual (premultiplied alpha) - DWM then composites
      // it over the sibling widgets beneath. (See d2d_build_composition_target.)
      bool input_transparent = wd.attrs &&
        wd.attrs->get_int(NEUI_ATTR_INPUT_TRANSPARENT, 0) != 0;
      bool overlay = wd.attrs &&
        wd.attrs->get_int(NEUI_ATTR_OVERLAY, 0) != 0;
      DWORD ex_style = 0;
      if (input_transparent) ex_style |= WS_EX_TRANSPARENT;
      if (overlay)           ex_style |= WS_EX_NOREDIRECTIONBITMAP;
      wd.paint_fn       = &paint_customdraw_w32;
      wd.painted_msg_fn = &painted_msg_customdraw_w32;
      HWND hwnd = CreateWindowExW(ex_style, L"neui.painted", L"", style,
        LogicalToPhysical(wd.x, parent_dpi),
        LogicalToPhysical(wd.y, parent_dpi),
        LogicalToPhysical(wd.width, parent_dpi),
        LogicalToPhysical(wd.height, parent_dpi),
        parent_hwnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(wd.index)),
        get_hinstance(),
        &wd);
      if (hwnd) {
        SetWindowSubclass(hwnd, ChildSubclassProc, 1, reinterpret_cast<DWORD_PTR>(&wd));
        wd.has_subclass = true;
        // An overlay must sit above the siblings it composites over. Child
        // z-order is creation order, so a client that adds the overlay last
        // already gets this; force it to the top so order-independence holds.
        if (overlay)
          SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      }
      return hwnd;
    }

    // GRID: shared painted-class HWND, paint_grid_w32 + painted_msg_grid_w32
    // do the heavy lifting via hosts/shared/widget_paint_grid.h. ChildSubclass
    // fires standard mouse / key events to the client (emit_events auto-set
    // above); the painted_msg hook runs after the client and dispatches the
    // GRID-specific event ladder (ROW_SELECTED -> CELL_SELECTED -> CELL_CLICKED).
    if (!strcmp(wd.type, NEUI_W_GRID)) {
      DWORD style = WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS;
      if (wd.visible) style |= WS_VISIBLE;
      wd.paint_fn       = &paint_grid_w32;
      wd.painted_msg_fn = &painted_msg_grid_w32;
      HWND hwnd = CreateWindowExW(0, L"neui.painted", L"", style,
        LogicalToPhysical(wd.x, parent_dpi),
        LogicalToPhysical(wd.y, parent_dpi),
        LogicalToPhysical(wd.width, parent_dpi),
        LogicalToPhysical(wd.height, parent_dpi),
        parent_hwnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(wd.index)),
        get_hinstance(),
        &wd);
      if (hwnd) {
        SetWindowSubclass(hwnd, ChildSubclassProc, 1, reinterpret_cast<DWORD_PTR>(&wd));
        wd.has_subclass = true;
      }
      return hwnd;
    }

    // TABVIEW: shared painted-class HWND drawing the chip strip + body. Its
    // TABPAGE children HWND-parent to it; paint_tabview_w32 lays out the
    // chips and tabview_relayout_w32 (run from create_child_windows once the
    // pages exist) sizes the selected page to the content body + hides the
    // rest. WS_TABSTOP so arrow-key tab nav works; WS_CLIPCHILDREN so the
    // body fill doesn't flicker under the page HWNDs.
    if (!strcmp(wd.type, NEUI_W_TABVIEW)) {
      DWORD style = WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
      if (wd.visible) style |= WS_VISIBLE;
      wd.paint_fn       = &paint_tabview_w32;
      wd.painted_msg_fn = &painted_msg_tabview_w32;
      wd.emit_events    = true;
      HWND hwnd = CreateWindowExW(0, L"neui.painted", L"", style,
        LogicalToPhysical(wd.x, parent_dpi),
        LogicalToPhysical(wd.y, parent_dpi),
        LogicalToPhysical(wd.width, parent_dpi),
        LogicalToPhysical(wd.height, parent_dpi),
        parent_hwnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(wd.index)),
        get_hinstance(),
        &wd);
      if (hwnd) wd.hwnd = hwnd;
      return hwnd;
    }

    // Section: non-interactive painted container by default; opts into
    // scrolling when NEUI_ATTR_SCROLL_MODE != "none". In that case an
    // inner "neui.sectionbody" HWND is created at the body rect; the
    // section's children HWND-parent to that body HWND so Win32's
    // default child-window clipping confines them to the body without
    // any per-child window regions. WS_CLIPCHILDREN on the section makes
    // its own paint skip the body HWND's pixels (we paint chip +
    // scrollbar; body_hwnd paints the body fill).
    // A TABPAGE is a chip-less SECTION and rides this exact path (its
    // effective chip text/align resolve to ""/"none", so band_h == 0 and
    // the body fills the rect).
    if (is_section_like_w32(wd.type)) {
      // Refresh the scroll state from any attrs the client set pre-show
      // so we know whether to opt into the scrolling code path.
      section_refresh_scroll_state_w32(wd);
      DWORD style = WS_CHILD | WS_CLIPSIBLINGS;
      if (wd.visible) style |= WS_VISIBLE;
      if (wd.section_scroll_state) style |= WS_CLIPCHILDREN;
      wd.paint_fn = &paint_section_w32;
      if (wd.section_scroll_state) {
        wd.painted_msg_fn = &painted_msg_section_w32;
        wd.emit_events    = true;
      }
      HWND hwnd = CreateWindowExW(0, L"neui.painted", L"", style,
        LogicalToPhysical(wd.x, parent_dpi),
        LogicalToPhysical(wd.y, parent_dpi),
        LogicalToPhysical(wd.width, parent_dpi),
        LogicalToPhysical(wd.height, parent_dpi),
        parent_hwnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(wd.index)),
        get_hinstance(),
        &wd);
      if (hwnd) {
        wd.hwnd = hwnd;
        apply_section_region_w32(wd);
        if (wd.section_scroll_state)
          section_create_body_hwnd_w32(wd);
      }
      return hwnd;
    }

    LPCWSTR wclass = nullptr;
    DWORD style = WS_CHILD;
    bool needs_subclass = false;

    for (auto& t : widget_type_table) {
      if (!strcmp(wd.type, t.neui_type)) {
        wclass = t.win32_class;
        style = t.style;
        needs_subclass = t.needs_subclass;
        break;
      }
    }

    if (!wclass) return nullptr;

    // Attribute-driven style overrides. The attribute bag is the authoritative
    // source for orthogonal features; widget-type strings only set defaults.
    if (wd.attrs) {
      if (!strcmp(wd.type, NEUI_W_CHECKBOX) &&
          wd.attrs->get_int(NEUI_ATTR_TRISTATE, 0))
      {
        style = (style & ~static_cast<DWORD>(BS_AUTOCHECKBOX)) | BS_AUTO3STATE;
      }
      if (!strcmp(wd.type, NEUI_W_SLIDER)) {
        const char* o = wd.attrs->get_string(NEUI_ATTR_ORIENTATION);
        if (o && !strcmp(o, "vertical"))
          style = (style & ~static_cast<DWORD>(TBS_HORZ)) | TBS_VERT;
        // Tick marks: NEUI_ATTR_STEPS >= 2 -> clear TBS_NOTICKS so user-
        // placed ticks render. We do NOT use TBS_AUTOTICKS because its
        // frequency (TBM_SETTICFREQ) must be integer; for non-divisible
        // step counts (e.g. 16 over a 1000 range) it rounds down and the
        // error compounds - visually the ticks drift left toward higher
        // values. We place each tick manually via TBM_SETTIC instead.
        if (wd.attrs->get_int(NEUI_ATTR_STEPS, 0) >= 2)
          style = (style & ~static_cast<DWORD>(TBS_NOTICKS));
      }
    }

    if (wd.visible) style |= WS_VISIBLE;

    HWND hwnd = CreateWindowExW(
      0,
      wclass,
      L"",
      style,
      LogicalToPhysical(wd.x, parent_dpi),
      LogicalToPhysical(wd.y, parent_dpi),
      LogicalToPhysical(wd.width, parent_dpi),
      LogicalToPhysical(wd.height, parent_dpi),
      parent_hwnd,
      reinterpret_cast<HMENU>(static_cast<UINT_PTR>(wd.index)),
      get_hinstance(),
      nullptr
    );

    if (hwnd) {
      ApplyText(hwnd, wd.text);
      if (needs_subclass) {
        SetWindowSubclass(hwnd, ChildSubclassProc, 1, reinterpret_cast<DWORD_PTR>(&wd));
        wd.has_subclass = true;
      }
      // Stash the HWND on the WidgetData before applying the visual
      // theme (apply_native_theme_w32 reads wd.hwnd to drive
      // SetWindowTheme + treeview colours).
      wd.hwnd = hwnd;
      apply_native_theme_w32(wd);
      // Slider: configure the trackbar's range and initial position from
      // NEUI_PARAM_VALUE. The internal range is 0..1000, exposed externally
      // as a normalized [0..1] float via NEUI_PARAM_VALUE.
      if (!strcmp(wd.type, NEUI_W_SLIDER)) {
        SendMessageW(hwnd, TBM_SETRANGE, FALSE, MAKELPARAM(0, 1000));
        int steps = wd.attrs ? wd.attrs->get_int(NEUI_ATTR_STEPS, 0) : 0;
        if (steps >= 2) {
          // Place each interior tick at the precisely-rounded position.
          // The endpoints are drawn automatically by the trackbar; we
          // only need the (steps - 2) interior ticks. Computing each
          // one as round(i / (steps-1) * 1000) avoids the cumulative
          // drift that TBM_SETTICFREQ produces with non-divisible counts.
          SendMessageW(hwnd, TBM_CLEARTICS, FALSE, 0);
          for (int i = 1; i < steps - 1; ++i) {
            int pos = static_cast<int>(
              static_cast<float>(i) /
              static_cast<float>(steps - 1) * 1000.0f + 0.5f);
            SendMessageW(hwnd, TBM_SETTIC, 0, pos);
          }
          // Arrow keys advance one step; Page keys advance two. The
          // WM_HSCROLL / WM_VSCROLL handler snaps the resulting position
          // to the exact step, so any rounding here is invisible.
          int line_size = 1000 / (steps - 1);
          if (line_size < 1) line_size = 1;
          SendMessageW(hwnd, TBM_SETLINESIZE, 0, line_size);
          SendMessageW(hwnd, TBM_SETPAGESIZE, 0, line_size * 2);
        } else {
          // Continuous mode: arrow keys move by 10% of the range, Page
          // keys by 20%. The trackbar's defaults (1 and 20) would mean
          // 100 arrow presses to traverse the slider, which is unusable.
          SendMessageW(hwnd, TBM_SETLINESIZE, 0, 100);
          SendMessageW(hwnd, TBM_SETPAGESIZE, 0, 200);
        }
        float v = clamp01_w32(wd.attrs ? wd.attrs->get_float(NEUI_PARAM_VALUE, 0.0f) : 0.0f);
        v = snap_to_steps_w32(v, steps);
        // Vertical: 1.0 = top in our convention, but Win32 default = 0 at top.
        bool vertical = (style & TBS_VERT) != 0;
        int  pos = static_cast<int>((vertical ? (1.0f - v) : v) * 1000.0f + 0.5f);
        SendMessageW(hwnd, TBM_SETPOS, TRUE, pos);
      }
    }

    return hwnd;
  }

  // Session member functions

  bool Session::run()
  {
    return win32_host::run();
  }

  WidgetData* Session::get_widget(uint32_t index)
  {
    if (_widgets.exists(index)) {
      return &_widgets[index];
    }
    return nullptr;
  }

  uint32_t Session::get_dpi_for_widget(uint32_t index)
  {
    if (!_widgets.exists(index)) return 96;
    auto& wd = _widgets[index];
    // Frame windows have their own DPI; check this widget first
    if (wd.hwnd && (wd.isroot || !strcmp(wd.type, NEUI_W_PLUGWINDOW))) {
      return wd.dpi;
    }
    // Walk parent chain to find the nearest frame window
    auto parents = _widgets.get_all_parents(index);
    for (auto p : parents) {
      if (p == 0) continue;
      if (_widgets.exists(p)) {
        auto& pd = _widgets[p];
        if (pd.hwnd && (pd.isroot || !strcmp(pd.type, NEUI_W_PLUGWINDOW))) {
          return pd.dpi;
        }
      }
    }
    return 96;
  }

  void Session::cascade_dpi(uint32_t parent_index, UINT new_dpi)
  {
    if (new_dpi == 0) new_dpi = 96;
    uint32_t child_idx = _widgets.child(parent_index);
    while (child_idx != 0) {
      if (_widgets.exists(child_idx)) {
        auto& wd = _widgets[child_idx];
        if (wd.hwnd) {
          wd.dpi = new_dpi;
          // Drop the cached DPI font on every container that has one
          // (typically painted widgets that act as parents - SECTION
          // most importantly). The follow-up create_child_windows will
          // recreate it at the new DPI; without this the stale font
          // would be re-broadcast to grandchildren via WM_SETFONT.
          if (wd.hfont) {
            DeleteObject(wd.hfont);
            wd.hfont = nullptr;
          }
          // Force a custom-font rebuild on the next ensure_custom_font_w32
          // call (signature is dpi-dependent). DeleteObject + null is the
          // cheapest way; the rebuild happens when create_child_windows
          // re-broadcasts the new parent font and we re-apply.
          if (wd.custom_hfont) {
            DeleteObject(wd.custom_hfont);
            wd.custom_hfont   = nullptr;
            wd.font_signature = 0;
          }
          // Reposition + resize this child to its logical geometry at the
          // new DPI. wd.x / wd.y are parent-relative logical pixels; the
          // new physical px land at the right spot in the parent HWND's
          // (already-resized) client area. If the parent is a scrolling
          // SECTION, subtract its scroll offset so the child's HWND lives
          // where the body-local coords say it does.
          int off_x = 0, off_y = 0;
          parent_scroll_offset_w32(this, child_idx, off_x, off_y);
          int new_x = LogicalToPhysical(wd.x - off_x, new_dpi);
          int new_y = LogicalToPhysical(wd.y - off_y, new_dpi);
          int new_w = LogicalToPhysical(wd.width,  new_dpi);
          int new_h = LogicalToPhysical(wd.height, new_dpi);
          SetWindowPos(wd.hwnd, nullptr, new_x, new_y, new_w, new_h,
                       SWP_NOZORDER | SWP_NOACTIVATE);
          // Painted widgets (KNOB, SECTION, IMAGE, CUSTOMDRAW) own a
          // per-widget D2D context whose DPI must be reset so DrawText
          // / fill_rect map logical -> physical at the new ratio. The
          // SetWindowPos above already fires WM_SIZE which handles the
          // swap-chain resize.
          auto* backend = neui_d2d_backend::get_backend();
          if (backend && backend->update_dpi && wd.paint_ctx)
            backend->update_dpi(wd.paint_ctx, new_dpi);
          // Section / TABPAGE: window region was computed in old physical
          // px; recompute against the new DPI. Also resize the inner body
          // HWND so its client rect tracks the body rect at the new DPI.
          if (is_section_like_w32(wd.type)) {
            apply_section_region_w32(wd);
            section_apply_layout_changes_w32(wd);
          }
          // TABVIEW: re-flow the chip strip + re-size the selected page at
          // the new DPI.
          if (wd.type && !strcmp(wd.type, NEUI_W_TABVIEW))
            tabview_relayout_w32(wd);
          // Image: re-pick the @Nx asset variant if the new DPI scale
          // crosses a boundary (no-op for client-owned assets).
          if (wd.type && !strcmp(wd.type, NEUI_W_IMAGE))
            reload_image_asset_for_dpi_w32(wd, new_dpi);
          // Native control refresh belt-and-suspenders: most native
          // controls auto-recompute row layout when WM_SETFONT arrives
          // from the follow-up create_child_windows pass, but a handful
          // need explicit messages or cache stale values across DPI
          // flips. These are cheap; no-op when the message is ignored.
          if (wd.type) {
            // TreeView: force item-height recompute from font. Without
            // this the rows can keep the old DPI's metrics until the
            // first item is added/removed.
            if (!strcmp(wd.type, NEUI_W_TREEVIEW))
              SendMessageW(wd.hwnd, TVM_SETITEMHEIGHT,
                           static_cast<WPARAM>(-1), 0);
            // ListBox / ComboBox: invalidate so the new font is picked
            // up across already-laid-out items. SetWindowPos above
            // invalidates on size change, but a same-size DPI flip
            // (rare, but possible across identical-DPI monitors with
            // different scaling preferences) wouldn't trigger it.
            if (!strcmp(wd.type, NEUI_W_LISTBOX)  ||
                !strcmp(wd.type, NEUI_W_COMBOBOX) ||
                !strcmp(wd.type, NEUI_W_MULTILINE))
              InvalidateRect(wd.hwnd, nullptr, TRUE);
          }
        }
        cascade_dpi(child_idx, new_dpi);
      }
      child_idx = _widgets.next(child_idx);
    }
  }

  void Session::create_child_windows(uint32_t parent_index)
  {
    if (!_widgets.exists(parent_index)) return;
    auto& parent_wd = _widgets[parent_index];
    // For scrolling SECTIONs, descendants HWND-parent to the inner body
    // HWND so Win32 clips them naturally to the body rect.
    HWND parent_hwnd = section_child_parent_hwnd_w32(parent_wd);
    UINT parent_dpi  = parent_wd.dpi;

    // Create a DPI-scaled font for this frame window on first use
    if (parent_wd.hfont == nullptr && parent_dpi > 0) {
      parent_wd.hfont = CreateDpiFont(parent_dpi);
    }

    uint32_t child_idx = _widgets.child(parent_index);
    while (child_idx != 0) {
      if (_widgets.exists(child_idx)) {
        auto& child = _widgets[child_idx];
        if (child.hmenu != nullptr && child.hwnd == nullptr) {
          // MENUBAR: attach to the parent frame window (HMENU was created on widget_create)
          SetMenu(parent_hwnd, child.hmenu);
          DrawMenuBar(parent_hwnd);
        } else if (child.hwnd == nullptr) {
          child.hwnd = CreateChildHwnd(child, parent_hwnd, parent_dpi);
          // Flush items buffered before the HWND existed (listbox/combobox)
          if (child.hwnd && !child.pending_items.empty()) {
            auto* ops = get_items_ops(child.type);
            if (ops) {
              for (auto& item : child.pending_items) {
                auto wtext = ToWide(item.text.c_str());
                LRESULT idx = SendMessageW(child.hwnd, ops->add, 0, (LPARAM)wtext.c_str());
                if (idx >= 0)
                  SendMessageW(child.hwnd, ops->setdata, (WPARAM)idx, (LPARAM)item.userdata);
              }
              child.pending_items.clear();
              if (child.pending_selection != NEUI_ITEM_NONE) {
                SendMessageW(child.hwnd, ops->setsel, (WPARAM)child.pending_selection, 0);
                child.pending_selection = NEUI_ITEM_NONE;
              }
            }
          }
          // Flush pending treeview items
          if (child.hwnd && !child.pending_tree_items.empty()) {
            for (auto& pitem : child.pending_tree_items) {
              auto it = child.tree_items.find(pitem.neui_id);
              if (it == child.tree_items.end()) continue;
              auto& data = it->second;

              HTREEITEM parent_hitem = TVI_ROOT;
              if (pitem.parent_neui_id != 0) {
                auto pit = child.tree_items.find(pitem.parent_neui_id);
                if (pit != child.tree_items.end())
                  parent_hitem = pit->second.hitem;
              }

              TVINSERTSTRUCTW tvis = {};
              tvis.hParent      = parent_hitem;
              tvis.hInsertAfter = TVI_LAST;
              tvis.item.mask    = TVIF_TEXT | TVIF_PARAM;
              auto wtext        = ToWide(data.text.c_str());
              tvis.item.pszText = const_cast<LPWSTR>(wtext.c_str());
              tvis.item.lParam  = static_cast<LPARAM>(pitem.neui_id);
              HTREEITEM hitem   = (HTREEITEM)SendMessageW(child.hwnd, TVM_INSERTITEMW, 0, (LPARAM)&tvis);
              data.hitem = hitem;
              if (hitem)
                child.tree_items_reverse[reinterpret_cast<uintptr_t>(hitem)] = pitem.neui_id;
            }
            child.pending_tree_items.clear();
          }
          // Flush pending check state for checkboxes
          if (child.pending_check >= 0) {
            SendMessageW(child.hwnd, BM_SETCHECK, (WPARAM)child.pending_check, 0);
            child.pending_check = -1;
          }
        }
        if (child.hwnd && parent_wd.hfont) {
          SendMessageW(child.hwnd, WM_SETFONT, (WPARAM)parent_wd.hfont, TRUE);
        }
        // Apply NEUI_ATTR_FONT_* if the client set them before show().
        // No-op when no font attrs are present.
        if (child.hwnd) ensure_custom_font_w32(child);
        // COMBOBOX: size the drop list from the item count + attrs now that
        // the font is in place. No-op for every other widget type. Runs here
        // (not just at create) so a DPI flip - which re-broadcasts the font
        // above and resets the HWND to its collapsed-only height in
        // cascade_dpi - re-grows the dropdown with correct metrics.
        if (child.hwnd) apply_combo_drop_sizing_w32(child);
        // Apply deferred enabled state. Children default to enabled=true;
        // only push to Win32 when the client toggled it before show().
        if (child.hwnd && !child.enabled)
          EnableWindow(child.hwnd, FALSE);
        // Re-pick the IMAGE asset variant if the parent's DPI scale
        // crossed a boundary since the slot was allocated (no-op for
        // client-owned assets).
        if (child.hwnd && child.type && !strcmp(child.type, NEUI_W_IMAGE))
          reload_image_asset_for_dpi_w32(child, parent_dpi);
        create_child_windows(child_idx);
      }
      child_idx = _widgets.next(child_idx);
    }
    // Scrolling SECTION: now that every child HWND exists, re-run the
    // layout helper so child positions are body-relative + scroll-shifted
    // and per-child clip regions cover the body rect. CreateChildHwnd
    // creates children at their raw (x, y) - correct for non-scrolling
    // sections, but a scrolling section needs the body / scroll offsets
    // applied and the chip + scrollbar areas masked out.
    if (parent_wd.type && is_section_like_w32(parent_wd.type) &&
        parent_wd.section_scroll_state) {
      section_apply_layout_changes_w32(parent_wd);
    }
    // TABVIEW: every TABPAGE child HWND (and their descendants) now exists -
    // lay out the chip strip, size the selected page to the content body
    // rect + hide the rest.
    if (parent_wd.type && !strcmp(parent_wd.type, NEUI_W_TABVIEW))
      tabview_relayout_w32(parent_wd);
  }

  neui_widget_t Session::widget_create(neui_widget_t parent, const char* type, int x, int y, int width, int height, void* userdata)
  {
    auto widget_data = std::make_unique<WidgetData>();
    widget_data->type        = type;
    widget_data->x           = x;
    widget_data->y           = y;
    widget_data->width       = width;
    widget_data->height      = height;
    widget_data->visible     = true;
    // Auto-enable events for widget types whose documented behaviour
    // depends on event delivery (selection notifications, value changes,
    // etc.). Matches the xpl host and the auto-set list in CLAUDE.md so
    // both hosts behave the same out of the box.
    widget_data->emit_events = type && (!strcmp(type, NEUI_W_BUTTON)     ||
                                        !strcmp(type, NEUI_W_INPUTBOX)   ||
                                        !strcmp(type, NEUI_W_CHECKBOX)   ||
                                        !strcmp(type, NEUI_W_CHECKBOX3)  ||
                                        !strcmp(type, NEUI_W_LISTBOX)    ||
                                        !strcmp(type, NEUI_W_COMBOBOX)   ||
                                        !strcmp(type, NEUI_W_MULTILINE)  ||
                                        !strcmp(type, NEUI_W_TREEVIEW)   ||
                                        !strcmp(type, NEUI_W_SLIDER)     ||
                                        !strcmp(type, NEUI_W_KNOB)       ||
                                        !strcmp(type, NEUI_W_CUSTOMDRAW) ||
                                        !strcmp(type, NEUI_W_GRID)       ||
                                        !strcmp(type, NEUI_W_TABVIEW));
    widget_data->userdata    = userdata;
    widget_data->session     = this;
    widget_data->session_id  = _session_id;

    // Map implicit type variants onto platform-neutral attributes so internal
    // behavior can be driven uniformly across hosts.
    if (type && !strcmp(type, NEUI_W_CHECKBOX3))
      neui_detail::ensure_attrs(widget_data->attrs).set_int(NEUI_ATTR_TRISTATE, 1);
    if (type && !strcmp(type, NEUI_W_MULTILINE))
      neui_detail::ensure_attrs(widget_data->attrs).set_int(NEUI_ATTR_MULTILINE, 1);

    neui_widget_t widget;

    if (parent.id == widget_none.id) {
      widget_data->isroot = true;
      uint32_t widget_id = _widgets.add_child(0, std::move(widget_data));
      _widgets[widget_id].index     = widget_id;
      _widgets[widget_id].widget_id = IndexToWidget(_session_id, widget_id).id;
      widget = IndexToWidget(_session_id, widget_id);
    } else {
      widget_data->isroot = false;
      uint32_t parent_index = WidgetToIndex(parent);
      uint32_t widget_id = _widgets.add_child(parent_index, std::move(widget_data));
      _widgets[widget_id].index     = widget_id;
      _widgets[widget_id].widget_id = IndexToWidget(_session_id, widget_id).id;
      widget = IndexToWidget(_session_id, widget_id);

      if (type && !strcmp(type, NEUI_W_MENUBAR)) {
        // MENUBAR: create the HMENU immediately; SetMenu is deferred to create_child_windows
        _widgets[widget_id].hmenu = CreateMenu();
        _menubars.push_back(widget_id);
        return widget;
      }

      // If parent HWND already exists, create child HWND immediately.
      // For scrolling SECTIONs, parent to the inner body HWND so Win32
      // clips children to the body rect naturally; then SetWindowPos at
      // body-local minus the current scroll so a child added mid-scroll
      // lands at the right spot. CreateChildHwnd uses (wd.x, wd.y) raw
      // which is correct only at scroll=0; the re-pos covers the rest.
      if (_widgets.exists(parent_index) && _widgets[parent_index].hwnd != nullptr) {
        UINT parent_dpi = get_dpi_for_widget(parent_index);
        auto& w = _widgets[widget_id];
        auto& parent_wd = _widgets[parent_index];
        HWND child_parent_hwnd = section_child_parent_hwnd_w32(parent_wd);
        w.hwnd = CreateChildHwnd(w, child_parent_hwnd, parent_dpi);
        if (w.hwnd && parent_wd.type &&
            is_section_like_w32(parent_wd.type) &&
            parent_wd.section_scroll_state) {
          section_compute_layout_w32(parent_wd);
          int off_x = 0, off_y = 0;
          parent_scroll_offset_w32(this, widget_id, off_x, off_y);
          SetWindowPos(w.hwnd, nullptr,
            LogicalToPhysical(w.x - off_x, parent_dpi),
            LogicalToPhysical(w.y - off_y, parent_dpi),
            LogicalToPhysical(w.width,  parent_dpi),
            LogicalToPhysical(w.height, parent_dpi),
            SWP_NOZORDER | SWP_NOACTIVATE);
          // Section repaint picks up the new scrollbar geometry (more
          // content -> thinner thumb).
          InvalidateRect(parent_wd.hwnd, nullptr, FALSE);
        }
        // A TABPAGE added to an already-shown TABVIEW: re-flow the strip +
        // re-apply page geometry (the new page must be sized to the body and
        // hidden unless it is the selected one). The page's own children get
        // created by the create_child_windows pass that the public API runs
        // after widget_create returns.
        else if (w.hwnd && parent_wd.type &&
                 !strcmp(parent_wd.type, NEUI_W_TABVIEW))
          tabview_relayout_w32(parent_wd);
      }
    }
    return widget;
  }

  void Session::widget_destroy(neui_widget_t widget)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return;

    // Destroy children first (depth-first, leaves before parent) so every
    // descendant's ondestroy is called before the parent's.
    // Save next-sibling before each recursive call because the call removes
    // the child from the sibling list, invalidating _widgets.next().
    {
      uint32_t child_idx = _widgets.child(index);
      while (child_idx != 0) {
        uint32_t next = _widgets.next(child_idx);
        if (_widgets.exists(child_idx))
          widget_destroy({ _widgets[child_idx].widget_id });
        child_idx = next;
      }
    }

    auto& w = _widgets[index];
    // Guard against a client_widget_api that left `ondestroy` null. The
    // public API allows clients to opt out of destroy notifications by
    // setting the function pointer to nullptr; this host has to honour
    // that rather than dereferencing it.
    if (_client_widget_api && _client_widget_api->ondestroy) {
      _client_widget_api->ondestroy(_token, widget, w.userdata);
    }
    if (w.has_subclass && w.hwnd) {
      RemoveWindowSubclass(w.hwnd, ChildSubclassProc, 1);
      w.has_subclass = false;
    }
    // IMAGE internal asset slot (if any) is the widget's responsibility
    // to release. Per-ctx GPU bitmaps cached against paint_ctx are
    // dropped by PaintedWndProc::WM_DESTROY via release_context.
    if (w.image_asset_owned && w.image_asset.id != asset_none.id) {
      _asset_manager.release_slot(w.image_asset.id & 0xffff,
                                   neui_d2d_backend::get_backend());
      w.image_asset       = asset_none;
      w.image_asset_owned = false;
    }
    if (w.hwnd) {
      // Revoke any IDropTarget that widget_show registered. Safe to call
      // on non-frame HWNDs (RevokeDragDrop returns DRAGDROP_E_NOTREGISTERED).
      if (w.isroot) RevokeDragDrop(w.hwnd);
      DestroyWindow(w.hwnd);
      w.hwnd = nullptr;
      // DestroyWindow cascades to descendants so the section's inner
      // body HWND is gone too. Null out the cached pointer to match.
      w.section_body_hwnd = nullptr;
    }
    if (w.hfont) {
      DeleteObject(w.hfont);
      w.hfont = nullptr;
    }
    if (w.custom_hfont) {
      DeleteObject(w.custom_hfont);
      w.custom_hfont   = nullptr;
      w.font_signature = 0;
    }
    if (w.native_icon) {
      DestroyIcon(w.native_icon);
      w.native_icon = nullptr;
    }
    if (w.section_ctl_bg_brush) {
      DeleteObject(w.section_ctl_bg_brush);
      w.section_ctl_bg_brush      = nullptr;
      w.section_ctl_bg_brush_argb = 0;
    }
    if (w.hmenu) {
      DestroyMenu(w.hmenu);
      w.hmenu = nullptr;
      _menubars.erase(std::remove(_menubars.begin(), _menubars.end(), index), _menubars.end());
    }
    if (w.native_accel) {
      DestroyAcceleratorTable(w.native_accel);
      w.native_accel = nullptr;
    }

    // A destroyed TABPAGE drops a tab: capture the parent TABVIEW so we can
    // re-flow its strip + page geometry after the slot is freed (the
    // selected index may now be out of range).
    uint32_t tabview_parent = 0;
    if (w.type && !strcmp(w.type, NEUI_W_TABPAGE)) {
      uint32_t pidx = _widgets.get_parent(index);
      if (pidx && _widgets.exists(pidx)) {
        auto& pw = _widgets[pidx];
        if (pw.type && !strcmp(pw.type, NEUI_W_TABVIEW)) tabview_parent = pidx;
      }
    }

    _widgets.remove(index);

    if (tabview_parent && _widgets.exists(tabview_parent))
      tabview_relayout_w32(_widgets[tabview_parent]);
  }

  void Session::widget_show(neui_widget_t widget)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return;
    auto& w = _widgets[index];
    w.visible = true;

    if (w.hwnd == nullptr && w.isroot) {
      // Deferred frame window creation on first show
      UINT initial_dpi = GetDpiForSystem();
      bool is_plug   = w.type && !strcmp(w.type, NEUI_W_PLUGWINDOW);
      bool is_dialog = w.type && !strcmp(w.type, NEUI_W_DIALOG);

      DWORD ex_style;
      DWORD style;
      const wchar_t* wclass;
      if (is_plug) {
        ex_style = 0;
        style    = WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        wclass   = L"neui.plugwindow";
      } else if (is_dialog) {
        ex_style = WS_EX_DLGMODALFRAME;
        style    = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        wclass   = L"neui.dialog";
      } else {
        ex_style = WS_EX_APPWINDOW;
        style    = WS_OVERLAPPEDWINDOW;
        wclass   = L"neui.appwindow";
      }

      // Resolve owner HWND for dialogs so CreateWindowEx can wire the
      // hwndParent slot correctly.
      HWND owner_hwnd = nullptr;
      if (is_dialog && w.owner_index != 0 && _widgets.exists(w.owner_index))
        owner_hwnd = _widgets[w.owner_index].hwnd;

      // Auto-centre the dialog over its owner when the client did not
      // request a specific position. (0, 0) is the conventional "no
      // position chosen" sentinel - clients that want a hard top-left
      // can call set_pos with explicit non-zero coords.
      int show_x_phys = LogicalToPhysical(w.x, initial_dpi);
      int show_y_phys = LogicalToPhysical(w.y, initial_dpi);
      int show_w_phys = LogicalToPhysical(w.width,  initial_dpi);
      int show_h_phys = LogicalToPhysical(w.height, initial_dpi);

      // neui's create() width/height specify the CLIENT (content) area - the
      // same contract as the macOS host (initWithContentRect:) and what
      // get_client_rect reports back. Win32's CreateWindowEx takes the OUTER
      // window size, so grow the requested client rect by the non-client
      // frame (title bar, resize borders, and the menu-bar row when this frame
      // carries a menubar) via AdjustWindowRectExForDpi. Without this the
      // usable client would be ~30-50 px shorter and a few px narrower than
      // asked for, clipping any widget a client laid out flush to the right /
      // bottom edge (e.g. a status bar pinned near the window height).
      {
        bool has_menu = false;
        for (uint32_t ci = _widgets.child(index); ci != 0; ci = _widgets.next(ci))
          if (_widgets.exists(ci) && _widgets[ci].hmenu != nullptr) { has_menu = true; break; }
        RECT wr = { 0, 0, show_w_phys, show_h_phys };
        AdjustWindowRectExForDpi(&wr, style, has_menu ? TRUE : FALSE, ex_style, initial_dpi);
        show_w_phys = wr.right - wr.left;
        show_h_phys = wr.bottom - wr.top;
      }

      if (is_dialog && owner_hwnd && w.x == 0 && w.y == 0) {
        RECT or_rc;
        if (GetWindowRect(owner_hwnd, &or_rc)) {
          show_x_phys = or_rc.left + ((or_rc.right  - or_rc.left) - show_w_phys) / 2;
          show_y_phys = or_rc.top  + ((or_rc.bottom - or_rc.top)  - show_h_phys) / 2;
          if (show_x_phys < 0) show_x_phys = 0;
          if (show_y_phys < 0) show_y_phys = 0;
        }
      }

      CreateWindowExW(
        ex_style,
        wclass,
        L"",
        style,
        show_x_phys,
        show_y_phys,
        show_w_phys,
        show_h_phys,
        owner_hwnd,
        nullptr,
        get_hinstance(),
        &w  // retrieved in WM_NCCREATE via CREATESTRUCT::lpCreateParams
      );
      // w.hwnd is set by AppWindowProc WM_NCCREATE; children created in WM_CREATE
      if (w.hwnd) {
        if (!is_plug) ApplyText(w.hwnd, w.text);
        // Apply any icon attribute set before show.
        if (w.attrs) {
          const char* icon_path = w.attrs->get_string(NEUI_ATTR_ICON_PATH);
          if (icon_path && *icon_path)
            neui_detail::apply_window_icon(w.hwnd, icon_path, &w.native_icon);
        }
        ShowWindow(w.hwnd, SW_SHOWNORMAL);
        UpdateWindow(w.hwnd);

        // Register the frame as a drop target so the OS routes external
        // drags to our IDropTarget. The widget's drop_target flag still
        // gates whether the client actually sees events.
        register_frame_as_drop_target_w32(w.hwnd, this, w.widget_id);

        // Dialog: block the owner's input while shown - unless the client
        // opted into modeless via NEUI_ATTR_MODAL = 0.
        bool is_modal = !w.attrs ||
                        w.attrs->get_int(NEUI_ATTR_MODAL, 1) != 0;
        if (is_dialog && owner_hwnd && is_modal)
          EnableWindow(owner_hwnd, FALSE);
        // Native blocking modal: spin a nested OS pump until the dialog
        // HWND is destroyed. AppWindowProc's WM_DESTROY for the dialog
        // clears modal_pump_active, breaking the pump so widget_show
        // returns to the caller.
        if (is_dialog && is_modal) {
          w.modal_pump_active = true;
          run_modal_until(&w.modal_pump_active);
        }
      }
    } else if (w.hwnd != nullptr) {
      ShowWindow(w.hwnd, SW_SHOW);
    }
  }

  void Session::widget_hide(neui_widget_t widget)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return;
    auto& w = _widgets[index];
    w.visible = false;
    if (w.hwnd) {
      ShowWindow(w.hwnd, SW_HIDE);
    }
  }

  void Session::widget_set_pos(neui_widget_t widget, int x, int y, int width, int height)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return;
    auto& w = _widgets[index];
    w.x = x; w.y = y; w.width = width; w.height = height;
    if (w.hwnd) {
      UINT dpi = get_dpi_for_widget(index);
      // If this widget is a child of a scrolling SECTION it lives inside
      // the section's body HWND - subtract the section's scroll so the
      // body-local (x, y) lands at the right body_hwnd-local px.
      int off_x = 0, off_y = 0;
      parent_scroll_offset_w32(this, index, off_x, off_y);
      SetWindowPos(w.hwnd, nullptr,
        LogicalToPhysical(x - off_x, dpi),
        LogicalToPhysical(y - off_y, dpi),
        LogicalToPhysical(width, dpi),
        LogicalToPhysical(height, dpi),
        SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // Section self-resize: rebuild layout + reposition own children.
    if (w.type && !strcmp(w.type, NEUI_W_SECTION))
      section_apply_layout_changes_w32(w);
  }

  void Session::widget_set_size(neui_widget_t widget, int width, int height)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return;
    auto& w = _widgets[index];
    w.width = width; w.height = height;
    if (w.hwnd) {
      UINT dpi = get_dpi_for_widget(index);
      SetWindowPos(w.hwnd, nullptr,
        0, 0,
        LogicalToPhysical(width, dpi),
        LogicalToPhysical(height, dpi),
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }

  void Session::widget_set_emit_events(neui_widget_t widget, bool enabled)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return;
    _widgets[index].emit_events = enabled;
  }

  void Session::widget_set_text(neui_widget_t widget, const char* text)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return;
    auto& w = _widgets[index];
    w.text = text ? text : "";
    if (w.type && !strcmp(w.type, NEUI_W_IMAGE)) {
      // IMAGE source: text path is allocated as an internally-owned asset
      // slot via the session's W32AssetManager. The slot is released on
      // re-set (here), on widget_set_asset, and on widget_destroy.
      // Client-supplied asset handles (image_asset_owned == false) are
      // simply dropped - the client still owns them via assets->destroy.
      auto* backend = neui_d2d_backend::get_backend();
      if (w.image_asset_owned && w.image_asset.id != asset_none.id) {
        _asset_manager.release_slot(w.image_asset.id & 0xffff, backend);
      }
      w.image_asset       = asset_none;
      w.image_asset_owned = false;

      if (!w.text.empty()) {
        float scale = static_cast<float>(get_dpi_for_widget(index)) / 96.0f;
        uint32_t slot = _asset_manager.allocate_from_file(w.text, scale);
        if (slot != 0) {
          w.image_asset       = pack_asset_w32(_session_id, slot);
          w.image_asset_owned = true;
        }
      }
      if (w.hwnd) InvalidateRect(w.hwnd, nullptr, FALSE);
    } else if (w.type && is_section_like_w32(w.type)) {
      // Section: text is the header title; chip width and the window
      // region both depend on it. Rebuild + repaint. (A TABPAGE resolves to
      // a chip-less full-rect region here.)
      if (w.hwnd) {
        apply_section_region_w32(w);
        InvalidateRect(w.hwnd, nullptr, FALSE);
      }
      // A TABPAGE's text is the tab label drawn by the parent TABVIEW - the
      // chip widths change, so re-flow the strip.
      if (auto* tv = tabview_parent_of_page_w32(w)) tabview_relayout_w32(*tv);
    } else {
      // Apply immediately if the HWND already exists; otherwise stored for deferred creation
      ApplyText(w.hwnd, w.text);
      // Programmatic mutation - break the typing run so the next user key
      // starts a fresh undo group. Don't push: API-driven changes shouldn't
      // pollute the user-visible undo stack.
      if (w.edit_history) w.edit_history->reset_action();
    }
  }

  int Session::widget_get_text(neui_widget_t widget, char* buf, int buflen)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return -1;
    auto& w = _widgets[index];

    // Sync from the Win32 control - captures user edits in INPUTBOX
    if (w.hwnd) {
      int wlen = GetWindowTextLengthW(w.hwnd);
      if (wlen > 0) {
        std::wstring wtext(wlen, L'\0');
        GetWindowTextW(w.hwnd, &wtext[0], wlen + 1);
        w.text = FromWide(wtext.c_str(), wlen);
      } else {
        w.text.clear();
      }
    }

    int needed = static_cast<int>(w.text.size()) + 1;  // bytes including null terminator
    if (buf && buflen > 0) {
      int copy = static_cast<int>(std::min(w.text.size(), static_cast<size_t>(buflen - 1)));
      memcpy(buf, w.text.c_str(), copy);
      buf[copy] = '\0';
    }
    return needed;
  }

  // Static C API wrapper functions

  static neui_widget_t create(neui_session_t session, neui_widget_t parent, const char* type, int x, int y, int width, int height, void* userdata)
  {
    auto s = get_session_for_widget(session, parent);
    if (s) return s->widget_create(parent, type, x, y, width, height, userdata);
    return { 0xFFFFFFFF };
  }

  // Instantiate a CUSTOMDRAW from a COMPONENT asset (create + attach compound +
  // behavior + stamp defaults). widget_set_asset's COMPONENT route does the
  // attach/stamp, so this is create() + set_asset, with default size fallback.
  static neui_widget_t NEUI_ABI create_from_component(neui_session_t session,
      neui_widget_t parent, neui_asset_t component, int x, int y, int width, int height)
  {
    // Validate the session via the parent handle (matches xpl + macOS parity).
    auto* s = get_session_for_widget(session, parent);
    if (!s || component.id == asset_none.id) return widget_none;
    if (((component.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return widget_none;
    auto* ce = s->_asset_manager.get_slot(component.id & 0xffff);
    if (!ce || ce->kind != NEUI_ASSET_KIND_COMPONENT) return widget_none;
    if (width  <= 0) width  = static_cast<int>(ce->comp_w + 0.5f);
    if (height <= 0) height = static_cast<int>(ce->comp_h + 0.5f);
    neui_widget_t w = create(session, parent, NEUI_W_CUSTOMDRAW, x, y, width, height, nullptr);
    if (w.id == widget_none.id) return widget_none;
    s->widget_set_asset(w, component);  // COMPONENT route: both slots + defaults
    return w;
  }

  static void destroy(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) s->widget_destroy(widget);
  }

  static void show(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) s->widget_show(widget);
  }

  static void hide(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) s->widget_hide(widget);
  }

  static void set_pos(neui_session_t session, neui_widget_t widget, int x, int y, int width, int height)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) s->widget_set_pos(widget, x, y, width, height);
  }

  static void set_size(neui_session_t session, neui_widget_t widget, int width, int height)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) s->widget_set_size(widget, width, height);
  }

  static void set_emit_events(neui_session_t session, neui_widget_t widget, bool enabled)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) s->widget_set_emit_events(widget, enabled);
  }

  neui_widget_t Session::widget_get_first_child(neui_widget_t widget)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return widget_none;
    uint32_t child = _widgets.child(index);
    if (child == 0 || !_widgets.exists(child)) return widget_none;
    return { _widgets[child].widget_id };
  }

  neui_widget_t Session::widget_get_next_sibling(neui_widget_t widget)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return widget_none;
    uint32_t next = _widgets.next(index);
    if (next == 0 || !_widgets.exists(next)) return widget_none;
    return { _widgets[next].widget_id };
  }

  static void set_text(neui_session_t session, neui_widget_t widget, const char* text)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) s->widget_set_text(widget, text);
  }

  static int get_text(neui_session_t session, neui_widget_t widget, char* buf, int buflen)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) return s->widget_get_text(widget, buf, buflen);
    return -1;
  }

  static neui_widget_t get_first_child(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) return s->widget_get_first_child(widget);
    return widget_none;
  }

  static neui_widget_t get_next_sibling(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) return s->widget_get_next_sibling(widget);
    return widget_none;
  }

  void Session::widget_set_focus(neui_widget_t widget)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return;
    auto& w = _widgets[index];
    if (w.hwnd) {
      SetFocus(w.hwnd);
    }
  }

  void Session::ensure_widget_visible(uint32_t widget_idx)
  {
    using namespace neui_detail;
    if (!_widgets.exists(widget_idx)) return;
    auto& wd0 = _widgets[widget_idx];
    int rect_x = wd0.x, rect_y = wd0.y;
    uint32_t cur = _widgets.get_parent(widget_idx);
    WidgetData* sec = nullptr;
    while (cur != 0 && cur != knone.id && _widgets.exists(cur)) {
      auto& cw = _widgets[cur];
      if (cw.section_scroll_state) { sec = &cw; break; }
      rect_x += cw.x;
      rect_y += cw.y;
      cur = _widgets.get_parent(cur);
    }
    if (!sec) return;
    auto& st = *sec->section_scroll_state;
    auto& L  = sec->section_last_layout;
    int nx, ny;
    compute_ensure_visible(rect_x, rect_y, wd0.width, wd0.height,
                            L.body_w, L.body_h,
                            st.content_w, st.content_h,
                            st.scroll_x, st.scroll_y,
                            nx, ny);
    section_external_commit_w32(*sec, nx, ny);
  }

  void Session::widget_set_owner(neui_widget_t dialog, neui_widget_t owner)
  {
    uint32_t didx = WidgetToIndex(dialog);
    if (!_widgets.exists(didx)) return;
    auto& d = _widgets[didx];
    if (!d.type || strcmp(d.type, NEUI_W_DIALOG) != 0) return;

    if (owner.id == widget_none.id) {
      d.owner_index = 0;
      return;
    }
    uint32_t oidx = WidgetToIndex(owner);
    if (!_widgets.exists(oidx)) return;
    const char* ot = _widgets[oidx].type;
    if (!ot || (strcmp(ot, NEUI_W_APPWINDOW) != 0 &&
                strcmp(ot, NEUI_W_PLUGWINDOW) != 0 &&
                strcmp(ot, NEUI_W_DIALOG)    != 0))
      return;
    d.owner_index = oidx;
  }

  void Session::close_owned_frames(uint32_t owner_idx)
  {
    if (owner_idx == 0) return;
    for (uint32_t idx : _widgets.release_order()) {
      if (idx == 0 || idx == owner_idx) continue;
      if (!_widgets.exists(idx)) continue;
      auto& other = _widgets[idx];
      if (other.owner_index == owner_idx && other.hwnd) {
        HWND child_hwnd = other.hwnd;
        other.hwnd = nullptr;
        DestroyWindow(child_hwnd);
      }
    }
  }

  void Session::update_menu_popup(HMENU popup)
  {
    if (!popup) return;
    bool has_validate = _menu_client && _menu_client->validate;
    for (uint32_t mb_idx : _menubars) {
      if (!_widgets.exists(mb_idx)) continue;
      auto& mw = _widgets[mb_idx];
      for (auto& kv : mw.menu_items) {
        uint32_t neui_id = kv.first;
        auto& mi = kv.second;
        if (mi.is_separator) continue;
        if (mi.parent_hmenu != popup) continue;
        // Re-assert the checkmark on every popup open: cheap and keeps the
        // state correct across any menu rebuild that recreated the HMENU item.
        if (!mi.submenu)
          CheckMenuItem(popup, mi.cmd_id,
                        MF_BYCOMMAND | (mi.checked ? MF_CHECKED : MF_UNCHECKED));
        if (!has_validate &&
            (mi.menu_cmd == 0 || mi.menu_cmd >= NEUI_CMD_USER_BASE))
          continue;  // nothing to compute for this item

        bool enabled = mi.enabled;
        if (enabled && mi.menu_cmd != 0 &&
            mi.menu_cmd < NEUI_CMD_USER_BASE) {
          enabled = can_focused_perform_command(mi.menu_cmd);
        }
        if (enabled && has_validate) {
          enabled = _menu_client->validate(
            _token, { mw.widget_id }, { neui_id }, mi.menu_cmd);
        }
        EnableMenuItem(popup, mi.cmd_id,
                        MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
      }
    }
  }

  HWND Session::find_parent_hwnd(uint32_t widget_index)
  {
    auto parents = _widgets.get_all_parents(widget_index);
    for (auto p : parents) {
      if (p == 0) continue;
      if (_widgets.exists(p) && _widgets[p].hwnd)
        return _widgets[p].hwnd;
    }
    return nullptr;
  }

  HFONT Session::find_parent_hfont(uint32_t widget_index)
  {
    auto parents = _widgets.get_all_parents(widget_index);
    for (auto p : parents) {
      if (p == 0) continue;
      if (_widgets.exists(p) && _widgets[p].hfont)
        return _widgets[p].hfont;
    }
    return nullptr;
  }

  bool Session::dispatch_menu_event(UINT cmd_id)
  {
    for (uint32_t mb_idx : _menubars) {
      if (!_widgets.exists(mb_idx)) continue;
      auto& mb = _widgets[mb_idx];
      auto it = mb.menu_cmd_map.find(cmd_id);
      if (it == mb.menu_cmd_map.end()) continue;
      uint32_t neui_id = it->second;

      // Built-in command routing: ask the focused control first.
      auto data_it = mb.menu_items.find(neui_id);
      if (data_it != mb.menu_items.end()) {
        uint32_t cmd = data_it->second.menu_cmd;
        if (cmd != 0 && cmd < NEUI_CMD_USER_BASE) {
          if (invoke_focused_command(cmd)) return true;
        }
      }

      neui_widget_t wid = { mb.widget_id };
      neui_event_t  ev  = { NEUI_EVENT_TREE_ITEM_ACTIVATED };
      ev.data.tree = { wid, { neui_id } };
      return dispatch_event(&ev);
    }
    return false;
  }

  // -------------------------------------------------------------------------
  // Commands API - uses GetFocus() to find the native edit and sends the
  // standard EM_*/WM_* messages. Other widget types are no-ops on Win32.

  // Returns true if hwnd is a native Edit control that knows how to handle
  // `cmd` - used both for the actual send and for the can-perform query.
  // UNDO / REDO go through our own EditHistory (lazy-allocated per widget
  // by the subclass-proc hook); the EDIT control's own EM_UNDO is a
  // single-level toggle, so we bypass it and replay state ourselves.
  static bool edit_can_handle(HWND hwnd, uint32_t cmd)
  {
    if (!hwnd) return false;
    wchar_t cls[32] = {};
    GetClassNameW(hwnd, cls, 32);
    if (lstrcmpiW(cls, L"Edit") != 0) return false;
    switch (cmd) {
    case NEUI_CMD_UNDO:
    case NEUI_CMD_REDO:
    case NEUI_CMD_CUT:
    case NEUI_CMD_COPY:
    case NEUI_CMD_PASTE:
    case NEUI_CMD_SELECT_ALL:
    case NEUI_CMD_DELETE:
      return true;
    }
    return false;
  }

  // Resolve a focused HWND back to its WidgetData, if it belongs to this
  // session. Used so UNDO / REDO can reach the per-widget EditHistory.
  static WidgetData* widget_for_hwnd_w32(Session* s, HWND hwnd)
  {
    if (!s || !hwnd) return nullptr;
    UINT id = (UINT)GetDlgCtrlID(hwnd);
    if (id == 0) return nullptr;
    WidgetData* wd = s->get_widget(id);
    if (wd && wd->hwnd == hwnd) return wd;
    return nullptr;
  }

  static bool send_text_command(Session* s, HWND hwnd, uint32_t cmd)
  {
    if (!edit_can_handle(hwnd, cmd)) return false;
    switch (cmd) {
    case NEUI_CMD_UNDO:
    case NEUI_CMD_REDO: {
      WidgetData* wd = widget_for_hwnd_w32(s, hwnd);
      if (wd && try_edit_undo_redo_w32(*wd, hwnd, cmd == NEUI_CMD_REDO))
        return true;
      // Fall back to the native single-level toggle for EDITs we don't
      // own (shouldn't happen for INPUTBOX/MULTILINE, but harmless).
      SendMessageW(hwnd, EM_UNDO, 0, 0);
      return true;
    }
    case NEUI_CMD_CUT:        SendMessageW(hwnd, WM_CUT,     0, 0);      return true;
    case NEUI_CMD_COPY:       SendMessageW(hwnd, WM_COPY,    0, 0);      return true;
    case NEUI_CMD_PASTE:      SendMessageW(hwnd, WM_PASTE,   0, 0);      return true;
    case NEUI_CMD_SELECT_ALL: SendMessageW(hwnd, EM_SETSEL,  0, -1);     return true;
    case NEUI_CMD_DELETE:     SendMessageW(hwnd, WM_CLEAR,   0, 0);      return true;
    }
    return false;
  }

  bool Session::invoke_focused_command(uint32_t cmd)
  {
    if (cmd == NEUI_CMD_NONE || cmd >= NEUI_CMD_USER_BASE) return false;
    return send_text_command(this, GetFocus(), cmd);
  }

  bool Session::invoke_command(neui_widget_t widget, uint32_t cmd)
  {
    if (cmd == NEUI_CMD_NONE || cmd >= NEUI_CMD_USER_BASE) return false;
    uint32_t idx = WidgetToIndex(widget);
    auto* w = get_widget(idx);
    if (!w) return false;
    return send_text_command(this, w->hwnd, cmd);
  }

  bool Session::can_focused_perform_command(uint32_t cmd)
  {
    if (cmd == NEUI_CMD_NONE || cmd >= NEUI_CMD_USER_BASE) return false;
    return edit_can_handle(GetFocus(), cmd);
  }

  static int NEUI_ABI cmd_invoke_focused(neui_session_t session, uint32_t cmd)
  {
    auto s = get_session(session);
    return (s && s->invoke_focused_command(cmd)) ? 1 : 0;
  }

  static int NEUI_ABI cmd_invoke(neui_session_t session, neui_widget_t widget,
                                  uint32_t cmd)
  {
    auto s = get_session_for_widget(session, widget);
    return (s && s->invoke_command(widget, cmd)) ? 1 : 0;
  }

  neui_commands_api_t commands_api = {
    NEUI_VERSION,
    cmd_invoke_focused,
    cmd_invoke,
  };

  // ---------------------------------------------------------------------------
  // Scroll API implementation
  // ---------------------------------------------------------------------------

  // External-commit shared by set_scroll + ensure_visible: writes the new
  // (nx, ny) into the section's state, resets the per-axis kinetics so a
  // later wheel doesn't spring back, repositions children + invalidates +
  // fires SCROLL_CHANGED.
  static void section_external_commit_w32(WidgetData& sec, int nx, int ny)
  {
    using namespace neui_detail;
    if (!sec.section_scroll_state || !sec.hwnd) return;
    auto& st = *sec.section_scroll_state;
    if (st.scroll_x == nx && st.scroll_y == ny) return;
    st.scroll_x = nx;
    st.scroll_y = ny;
    st.kin_v.raw_px            = (double)ny;
    st.kin_v.last_commit_px    = ny;
    st.kin_v.suppress_momentum = true;
    st.kin_h.raw_px            = (double)nx;
    st.kin_h.last_commit_px    = nx;
    st.kin_h.suppress_momentum = true;
    st.kinetic_over_v = false;
    st.kinetic_over_h = false;
    // Cancel any spring-back so a programmatic set isn't fought by an
    // in-flight bounce timer.
    KillTimer(sec.hwnd, SECTION_BOUNCE_TIMER_ID);
    section_reposition_children_w32(sec);
    InvalidateRect(sec.hwnd, nullptr, FALSE);
    section_notify_scroll_changed_w32(sec);
  }

  static int scroll_set(neui_session_t session, neui_widget_t widget,
                         int scroll_x, int scroll_y)
  {
    auto s = get_session_for_widget(session, widget);
    if (!s) return 0;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return 0;
    auto& wd = s->_widgets[idx];
    if (!wd.section_scroll_state) return 0;
    auto& st = *wd.section_scroll_state;
    auto& L  = wd.section_last_layout;
    int max_x = st.content_w - L.body_w; if (max_x < 0) max_x = 0;
    int max_y = st.content_h - L.body_h; if (max_y < 0) max_y = 0;
    int nx = scroll_x; if (nx < 0) nx = 0; if (nx > max_x) nx = max_x;
    int ny = scroll_y; if (ny < 0) ny = 0; if (ny > max_y) ny = max_y;
    section_external_commit_w32(wd, nx, ny);
    return 1;
  }

  static int scroll_get(neui_session_t session, neui_widget_t widget,
                         int* out_x, int* out_y)
  {
    auto s = get_session_for_widget(session, widget);
    if (!s) return 0;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return 0;
    auto& wd = s->_widgets[idx];
    if (!wd.section_scroll_state) return 0;
    if (out_x) *out_x = wd.section_scroll_state->scroll_x;
    if (out_y) *out_y = wd.section_scroll_state->scroll_y;
    return 1;
  }

  static int scroll_ensure_visible(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget);
    if (!s) return 0;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return 0;
    s->ensure_widget_visible(idx);
    return 1;
  }

  neui_scroll_api_t scroll_api = {
    NEUI_VERSION,
    scroll_set,
    scroll_get,
    scroll_ensure_visible,
  };

  // ---------------------------------------------------------------------------
  // Tabs API (NEUI_API_TABS) - selection control over a TABVIEW. Tabs are the
  // TABVIEW's NEUI_W_TABPAGE children in creation order. Mirror of the xpl /
  // macOS hosts' tabview_from + the 5 thin methods.
  // ---------------------------------------------------------------------------

  // Resolve `widget` to a TABVIEW WidgetData* (valid, this session, type
  // TABVIEW) or nullptr.
  static WidgetData* tabview_from_w32(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return nullptr;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return nullptr;
    auto& wd = s->_widgets[idx];
    if (!wd.type || strcmp(wd.type, NEUI_W_TABVIEW) != 0) return nullptr;
    return &wd;
  }

  static uint32_t NEUI_ABI tabs_count(neui_session_t session, neui_widget_t tabview)
  {
    WidgetData* tv = tabview_from_w32(session, tabview);
    if (!tv) return 0;
    std::vector<uint32_t> pages; tabview_collect_pages_w32(*tv, pages);
    return static_cast<uint32_t>(pages.size());
  }

  static uint32_t NEUI_ABI tabs_get_selected(neui_session_t session, neui_widget_t tabview)
  {
    WidgetData* tv = tabview_from_w32(session, tabview);
    if (!tv) return NEUI_ITEM_NONE;
    std::vector<uint32_t> pages; tabview_collect_pages_w32(*tv, pages);
    if (pages.empty()) return NEUI_ITEM_NONE;
    int sel = tv->tab_selected;
    if (sel < 0) sel = 0;
    if (sel >= static_cast<int>(pages.size())) sel = static_cast<int>(pages.size()) - 1;
    return static_cast<uint32_t>(sel);
  }

  static void NEUI_ABI tabs_set_selected(neui_session_t session, neui_widget_t tabview,
                                         uint32_t index)
  {
    WidgetData* tv = tabview_from_w32(session, tabview);
    if (!tv) return;
    // Clamp huge / sentinel indices (e.g. NEUI_ITEM_NONE) to a representable
    // int so the cast doesn't wrap negative - per the documented "clamped to
    // [0, count)", an out-of-range index selects the LAST tab, not the first.
    int ni = index > 0x7fffffffu ? 0x7fffffff : static_cast<int>(index);
    tabview_select_w32(*tv, ni);
  }

  static neui_widget_t NEUI_ABI tabs_get_page(neui_session_t session,
                                              neui_widget_t tabview, uint32_t index)
  {
    WidgetData* tv = tabview_from_w32(session, tabview);
    if (!tv) return widget_none;
    std::vector<uint32_t> pages; tabview_collect_pages_w32(*tv, pages);
    if (index >= pages.size()) return widget_none;
    return IndexToWidget(tv->session->session_id(), pages[index]);
  }

  static uint32_t NEUI_ABI tabs_get_index(neui_session_t session, neui_widget_t tabview,
                                          neui_widget_t page)
  {
    WidgetData* tv = tabview_from_w32(session, tabview);
    if (!tv) return NEUI_ITEM_NONE;
    uint32_t page_idx = WidgetToIndex(page);
    std::vector<uint32_t> pages; tabview_collect_pages_w32(*tv, pages);
    for (uint32_t i = 0; i < pages.size(); ++i)
      if (pages[i] == page_idx) return i;
    return NEUI_ITEM_NONE;
  }

  neui_tabs_api_t tabs_api = {
    NEUI_VERSION,
    tabs_count,
    tabs_get_selected,
    tabs_set_selected,
    tabs_get_page,
    tabs_get_index,
  };

  // ---------------------------------------------------------------------------
  // Items API implementation
  // ---------------------------------------------------------------------------

  void Session::items_clear(neui_widget_t widget)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return;
    auto& w = _widgets[index];
    auto* ops = get_items_ops(w.type);
    if (!ops || !w.hwnd) return;
    SendMessageW(w.hwnd, ops->clear, 0, 0);
    apply_combo_drop_sizing_w32(w);
  }

  uint32_t Session::items_add(neui_widget_t widget, const char* text, void* userdata)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return NEUI_ITEM_NONE;
    auto& w = _widgets[index];
    auto* ops = get_items_ops(w.type);
    if (!ops) return NEUI_ITEM_NONE;
    if (!w.hwnd) {
      // HWND not yet created - buffer for deferred application on show
      w.pending_items.push_back({ text ? text : "", userdata });
      return static_cast<uint32_t>(w.pending_items.size() - 1);
    }
    auto wtext = ToWide(text ? text : "");
    LRESULT idx = SendMessageW(w.hwnd, ops->add, 0, (LPARAM)wtext.c_str());
    if (idx < 0) return NEUI_ITEM_NONE;
    SendMessageW(w.hwnd, ops->setdata, (WPARAM)idx, (LPARAM)userdata);
    apply_combo_drop_sizing_w32(w);
    return static_cast<uint32_t>(idx);
  }

  void Session::items_remove(neui_widget_t widget, uint32_t index)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return;
    auto& w = _widgets[wi];
    auto* ops = get_items_ops(w.type);
    if (!ops || !w.hwnd) return;
    SendMessageW(w.hwnd, ops->del, (WPARAM)index, 0);
    apply_combo_drop_sizing_w32(w);
  }

  uint32_t Session::items_count(neui_widget_t widget)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return 0;
    auto& w = _widgets[index];
    auto* ops = get_items_ops(w.type);
    if (!ops || !w.hwnd) return 0;
    LRESULT n = SendMessageW(w.hwnd, ops->count, 0, 0);
    return (n < 0) ? 0 : static_cast<uint32_t>(n);
  }

  int Session::items_get_text(neui_widget_t widget, uint32_t index, char* buf, int buflen)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return -1;
    auto& w = _widgets[wi];
    auto* ops = get_items_ops(w.type);
    if (!ops || !w.hwnd) return -1;
    LRESULT wlen = SendMessageW(w.hwnd, ops->textlen, (WPARAM)index, 0);
    if (wlen < 0) return -1;
    std::wstring wbuf(static_cast<size_t>(wlen), L'\0');
    SendMessageW(w.hwnd, ops->gettext, (WPARAM)index, (LPARAM)&wbuf[0]);
    std::string utf8 = FromWide(wbuf.c_str(), static_cast<int>(wlen));
    int needed = static_cast<int>(utf8.size()) + 1;
    if (buf && buflen > 0) {
      int copy = static_cast<int>(std::min(utf8.size(), static_cast<size_t>(buflen - 1)));
      memcpy(buf, utf8.c_str(), copy);
      buf[copy] = '\0';
    }
    return needed;
  }

  void Session::items_set_text(neui_widget_t widget, uint32_t index, const char* text)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return;
    auto& w = _widgets[wi];
    auto* ops = get_items_ops(w.type);
    if (!ops || !w.hwnd) return;
    // Preserve userdata across delete+reinsert
    LRESULT udata = SendMessageW(w.hwnd, ops->getdata, (WPARAM)index, 0);
    SendMessageW(w.hwnd, ops->del, (WPARAM)index, 0);
    auto wtext = ToWide(text ? text : "");
    SendMessageW(w.hwnd, ops->insert, (WPARAM)index, (LPARAM)wtext.c_str());
    SendMessageW(w.hwnd, ops->setdata, (WPARAM)index, (LPARAM)udata);
  }

  void* Session::items_get_userdata(neui_widget_t widget, uint32_t index)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return nullptr;
    auto& w = _widgets[wi];
    auto* ops = get_items_ops(w.type);
    if (!ops || !w.hwnd) return nullptr;
    return reinterpret_cast<void*>(SendMessageW(w.hwnd, ops->getdata, (WPARAM)index, 0));
  }

  uint32_t Session::items_get_selected(neui_widget_t widget)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return NEUI_ITEM_NONE;
    auto& w = _widgets[index];
    auto* ops = get_items_ops(w.type);
    if (!ops || !w.hwnd) return NEUI_ITEM_NONE;
    LRESULT sel = SendMessageW(w.hwnd, ops->getsel, 0, 0);
    return (sel < 0) ? NEUI_ITEM_NONE : static_cast<uint32_t>(sel);
  }

  void Session::items_set_selected(neui_widget_t widget, uint32_t index)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return;
    auto& w = _widgets[wi];
    auto* ops = get_items_ops(w.type);
    if (!ops) return;
    if (!w.hwnd) {
      w.pending_selection = index;
      return;
    }
    WPARAM item = (index == NEUI_ITEM_NONE) ? (WPARAM)-1 : (WPARAM)index;
    SendMessageW(w.hwnd, ops->setsel, item, 0);
  }

  // ---------------------------------------------------------------------------
  // Items API static wrappers
  // ---------------------------------------------------------------------------

  static void items_clear_fn(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget); if (s) s->items_clear(widget);
  }
  static uint32_t items_add_fn(neui_session_t session, neui_widget_t widget, const char* text, void* userdata)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->items_add(widget, text, userdata) : NEUI_ITEM_NONE;
  }
  static void items_remove_fn(neui_session_t session, neui_widget_t widget, uint32_t index)
  {
    auto s = get_session_for_widget(session, widget); if (s) s->items_remove(widget, index);
  }
  static uint32_t items_count_fn(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->items_count(widget) : 0;
  }
  static int items_get_text_fn(neui_session_t session, neui_widget_t widget, uint32_t index, char* buf, int buflen)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->items_get_text(widget, index, buf, buflen) : -1;
  }
  static void items_set_text_fn(neui_session_t session, neui_widget_t widget, uint32_t index, const char* text)
  {
    auto s = get_session_for_widget(session, widget); if (s) s->items_set_text(widget, index, text);
  }
  static void* items_get_userdata_fn(neui_session_t session, neui_widget_t widget, uint32_t index)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->items_get_userdata(widget, index) : nullptr;
  }
  static uint32_t items_get_selected_fn(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->items_get_selected(widget) : NEUI_ITEM_NONE;
  }
  static void items_set_selected_fn(neui_session_t session, neui_widget_t widget, uint32_t index)
  {
    auto s = get_session_for_widget(session, widget); if (s) s->items_set_selected(widget, index);
  }

  neui_items_api_t items_api = {
    items_clear_fn,
    items_add_fn,
    items_remove_fn,
    items_count_fn,
    items_get_text_fn,
    items_set_text_fn,
    items_get_userdata_fn,
    items_get_selected_fn,
    items_set_selected_fn,
  };

  // ---------------------------------------------------------------------------
  // Tree API implementation
  // ---------------------------------------------------------------------------

  static bool is_treeview(const char* type) { return type && !strcmp(type, NEUI_W_TREEVIEW); }
  static bool is_menubar (const char* type) { return type && !strcmp(type, NEUI_W_MENUBAR);  }

  neui_item_t Session::tree_add(neui_widget_t widget, neui_item_t parent, const char* text, void* userdata)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return tree_item_none;
    auto& w = _widgets[wi];
    const char* txt = text ? text : "";

    if (is_treeview(w.type)) {
      uint32_t neui_id = w.next_tree_id++;
      WidgetData::TreeItemData data;
      data.userdata = userdata;
      data.text     = txt;
      data.enabled  = true;
      data.hitem    = nullptr;

      if (!w.hwnd) {
        // Buffer until HWND is created
        w.tree_items[neui_id] = std::move(data);
        w.pending_tree_items.push_back({ neui_id, parent.id });
        return { neui_id };
      }
      HTREEITEM parent_hitem = TVI_ROOT;
      if (parent.id != 0) {
        auto it = w.tree_items.find(parent.id);
        if (it != w.tree_items.end()) parent_hitem = it->second.hitem;
      }
      TVINSERTSTRUCTW tvis   = {};
      tvis.hParent           = parent_hitem;
      tvis.hInsertAfter      = TVI_LAST;
      tvis.item.mask         = TVIF_TEXT | TVIF_PARAM;
      auto wtext             = ToWide(txt);
      tvis.item.pszText      = const_cast<LPWSTR>(wtext.c_str());
      tvis.item.lParam       = static_cast<LPARAM>(neui_id);
      HTREEITEM hitem        = (HTREEITEM)SendMessageW(w.hwnd, TVM_INSERTITEMW, 0, (LPARAM)&tvis);
      data.hitem             = hitem;
      w.tree_items[neui_id]  = std::move(data);
      if (hitem) w.tree_items_reverse[reinterpret_cast<uintptr_t>(hitem)] = neui_id;
      return { neui_id };
    }

    if (is_menubar(w.type)) {
      uint32_t neui_id = w.next_menu_item_id++;
      WidgetData::MenuItemData data;
      data.text           = txt;
      data.enabled        = true;
      data.userdata       = userdata;
      data.parent_item_id = parent.id;

      if (parent.id == 0) {
        // Top-level menu bar item - create a popup submenu for it
        HMENU popup = CreatePopupMenu();
        data.parent_hmenu  = w.hmenu;
        data.submenu       = popup;
        data.cmd_id        = 0;
        auto wtext         = ToWide(txt);
        AppendMenuW(w.hmenu, MF_POPUP, reinterpret_cast<UINT_PTR>(popup), wtext.c_str());
      } else {
        auto pit = w.menu_items.find(parent.id);
        HMENU parent_popup = (pit != w.menu_items.end() && pit->second.submenu) ? pit->second.submenu : w.hmenu;
        // Allocate from the free list first so a churning menubar (eg. a
        // dynamic MIDI device list) doesn't exhaust the 16-bit cmd_id
        // space. Past 0xFFFF the WM_COMMAND wParam LOWORD would alias
        // earlier cmd_ids and silently corrupt dispatch.
        UINT cmd_id;
        if (!w.free_menu_cmd_ids.empty()) {
          cmd_id = w.free_menu_cmd_ids.back();
          w.free_menu_cmd_ids.pop_back();
        } else {
          assert(w.next_menu_cmd_id <= 0xFFFF &&
                 "menubar exhausted 16-bit cmd_id range; tree_remove items first");
          cmd_id = w.next_menu_cmd_id++;
        }
        data.parent_hmenu  = parent_popup;
        data.submenu       = nullptr;
        data.cmd_id        = cmd_id;

        if (strcmp(txt, "-") == 0) {
          // Separator - use InsertMenuItemW with MIIM_ID so cmd_id is stored and MF_BYCOMMAND removal works
          data.is_separator = true;
          MENUITEMINFOW mii = {};
          mii.cbSize = sizeof(mii);
          mii.fMask  = MIIM_TYPE | MIIM_ID;
          mii.fType  = MFT_SEPARATOR;
          mii.wID    = cmd_id;
          InsertMenuItemW(parent_popup, GetMenuItemCount(parent_popup), TRUE, &mii);
        } else {
          // Normal command item
          auto wtext = ToWide(txt);
          AppendMenuW(parent_popup, MF_STRING, cmd_id, wtext.c_str());
          w.menu_cmd_map[cmd_id] = neui_id;
        }
      }
      w.menu_items[neui_id] = std::move(data);
      w.menu_item_ids_ordered.push_back(neui_id);
      // If menu bar is already attached, refresh it
      HWND frame = find_parent_hwnd(wi);
      if (frame) DrawMenuBar(frame);
      return { neui_id };
    }
    return tree_item_none;
  }

  // Forward decl - body is below near tree_set_shortcut.
  static void rebuild_menubar_accel_w32(WidgetData& mb);

  void Session::tree_remove(neui_widget_t widget, neui_item_t item)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return;
    auto& w = _widgets[wi];

    if (is_treeview(w.type)) {
      auto it = w.tree_items.find(item.id);
      if (it == w.tree_items.end()) return;
      if (w.hwnd && it->second.hitem) {
        // TreeView_DeleteItem also removes children
        w.tree_items_reverse.erase(reinterpret_cast<uintptr_t>(it->second.hitem));
        TreeView_DeleteItem(w.hwnd, it->second.hitem);
      }
      w.tree_items.erase(it);
      return;
    }

    if (is_menubar(w.type)) {
      auto it = w.menu_items.find(item.id);
      if (it == w.menu_items.end()) return;
      auto& data = it->second;
      bool had_shortcut = (data.shortcut_key != NEUI_KEY_NONE);
      if (data.submenu) {
        // Submenu matched by its HMENU value as the MF_BYCOMMAND id; the
        // position arg is UINT, so truncate to the low 32 bits explicitly.
        RemoveMenu(data.parent_hmenu,
                   static_cast<UINT>(reinterpret_cast<UINT_PTR>(data.submenu)),
                   MF_BYCOMMAND);
        DestroyMenu(data.submenu);
      } else {
        RemoveMenu(data.parent_hmenu, data.cmd_id, MF_BYCOMMAND);
        w.menu_cmd_map.erase(data.cmd_id);
        // Recycle the cmd_id so the next tree_add can claim it before
        // bumping next_menu_cmd_id (keeps a churning menubar from
        // exhausting the 16-bit cmd_id range).
        w.free_menu_cmd_ids.push_back(data.cmd_id);
      }
      w.menu_item_ids_ordered.erase(
        std::remove(w.menu_item_ids_ordered.begin(), w.menu_item_ids_ordered.end(), item.id),
        w.menu_item_ids_ordered.end());
      w.menu_items.erase(it);
      if (had_shortcut) rebuild_menubar_accel_w32(w);
      HWND frame = find_parent_hwnd(wi);
      if (frame) DrawMenuBar(frame);
    }
  }

  void Session::tree_clear(neui_widget_t widget)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return;
    auto& w = _widgets[wi];

    if (is_treeview(w.type)) {
      if (w.hwnd) TreeView_DeleteAllItems(w.hwnd);
      w.tree_items.clear();
      w.tree_items_reverse.clear();
      w.pending_tree_items.clear();
      w.next_tree_id = 1;
      return;
    }

    if (is_menubar(w.type)) {
      // Destroy and recreate the menu
      for (auto& pair : w.menu_items) {
        if (pair.second.submenu) DestroyMenu(pair.second.submenu);
      }
      w.menu_items.clear();
      w.menu_cmd_map.clear();
      w.menu_item_ids_ordered.clear();
      w.next_menu_item_id = 1;
      w.next_menu_cmd_id  = 0x8000;
      w.free_menu_cmd_ids.clear();
      if (w.native_accel) {
        DestroyAcceleratorTable(w.native_accel);
        w.native_accel = nullptr;
      }
      // Repopulate with a fresh menu
      DestroyMenu(w.hmenu);
      w.hmenu = CreateMenu();
      HWND frame = find_parent_hwnd(wi);
      if (frame) { SetMenu(frame, w.hmenu); DrawMenuBar(frame); }
    }
  }

  int Session::tree_get_text(neui_widget_t widget, neui_item_t item, char* buf, int buflen)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return -1;
    auto& w = _widgets[wi];

    if (is_treeview(w.type)) {
      auto it = w.tree_items.find(item.id);
      if (it == w.tree_items.end()) return -1;
      // Sync from the Win32 control if HWND exists
      if (w.hwnd && it->second.hitem) {
        wchar_t wbuf[512] = {};
        TVITEMW tvi = {};
        tvi.hItem      = it->second.hitem;
        tvi.mask       = TVIF_TEXT;
        tvi.pszText    = wbuf;
        tvi.cchTextMax = 512;
        SendMessageW(w.hwnd, TVM_GETITEMW, 0, (LPARAM)&tvi);
        it->second.text = FromWide(wbuf, static_cast<int>(wcslen(wbuf)));
      }
      const std::string& s = it->second.text;
      int needed = static_cast<int>(s.size()) + 1;
      if (buf && buflen > 0) {
        int copy = static_cast<int>(std::min(s.size(), static_cast<size_t>(buflen - 1)));
        memcpy(buf, s.c_str(), copy);
        buf[copy] = '\0';
      }
      return needed;
    }

    if (is_menubar(w.type)) {
      auto it = w.menu_items.find(item.id);
      if (it == w.menu_items.end()) return -1;
      const std::string s = it->second.is_separator ? std::string("-") : it->second.text;
      int needed = static_cast<int>(s.size()) + 1;
      if (buf && buflen > 0) {
        int copy = static_cast<int>(std::min(s.size(), static_cast<size_t>(buflen - 1)));
        memcpy(buf, s.c_str(), copy);
        buf[copy] = '\0';
      }
      return needed;
    }
    return -1;
  }

  void Session::tree_set_text(neui_widget_t widget, neui_item_t item, const char* text)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return;
    auto& w = _widgets[wi];
    const char* txt = text ? text : "";

    if (is_treeview(w.type)) {
      auto it = w.tree_items.find(item.id);
      if (it == w.tree_items.end()) return;
      it->second.text = txt;
      if (w.hwnd && it->second.hitem) {
        auto wtext = ToWide(txt);
        TVITEMW tvi = {};
        tvi.hItem    = it->second.hitem;
        tvi.mask     = TVIF_TEXT;
        tvi.pszText  = const_cast<LPWSTR>(wtext.c_str());
        SendMessageW(w.hwnd, TVM_SETITEMW, 0, (LPARAM)&tvi);
      }
      return;
    }

    if (is_menubar(w.type)) {
      auto it = w.menu_items.find(item.id);
      if (it == w.menu_items.end()) return;
      it->second.text = txt;
      std::string display = txt;
      if (!it->second.shortcut.empty()) display += '\t' + it->second.shortcut;
      auto wtext = ToWide(display.c_str());
      UINT flags = (it->second.submenu ? MF_POPUP : MF_STRING);
      UINT_PTR id_or_menu = it->second.submenu ? reinterpret_cast<UINT_PTR>(it->second.submenu)
                                                : static_cast<UINT_PTR>(it->second.cmd_id);
      // uPosition (2nd arg) is UINT - take the low 32 bits of the command id;
      // uIDNewItem (4th arg) is UINT_PTR and keeps the full value.
      ModifyMenuW(it->second.parent_hmenu, static_cast<UINT>(id_or_menu),
                  flags | MF_BYCOMMAND, id_or_menu, wtext.c_str());
      HWND frame = find_parent_hwnd(wi);
      if (frame) DrawMenuBar(frame);
    }
  }

  void* Session::tree_get_userdata(neui_widget_t widget, neui_item_t item)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return nullptr;
    auto& w = _widgets[wi];

    if (is_treeview(w.type)) {
      auto it = w.tree_items.find(item.id);
      return (it != w.tree_items.end()) ? it->second.userdata : nullptr;
    }
    if (is_menubar(w.type)) {
      auto it = w.menu_items.find(item.id);
      return (it != w.menu_items.end()) ? it->second.userdata : nullptr;
    }
    return nullptr;
  }

  void Session::tree_set_enabled(neui_widget_t widget, neui_item_t item, bool enabled)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return;
    auto& w = _widgets[wi];

    if (is_treeview(w.type)) {
      auto it = w.tree_items.find(item.id);
      if (it != w.tree_items.end()) it->second.enabled = enabled;
      return;
    }
    if (is_menubar(w.type)) {
      auto it = w.menu_items.find(item.id);
      if (it == w.menu_items.end() || it->second.is_separator) return;
      it->second.enabled = enabled;
      UINT flags = enabled ? MF_ENABLED : MF_GRAYED;
      if (it->second.submenu) {
        // Popup item: find its position in the parent menu (HMENU is 64-bit; can't use MF_BYCOMMAND with UINT)
        int count = GetMenuItemCount(it->second.parent_hmenu);
        for (int pos = 0; pos < count; ++pos) {
          if (GetSubMenu(it->second.parent_hmenu, pos) == it->second.submenu) {
            EnableMenuItem(it->second.parent_hmenu, static_cast<UINT>(pos), MF_BYPOSITION | flags);
            break;
          }
        }
      } else {
        EnableMenuItem(it->second.parent_hmenu, it->second.cmd_id, MF_BYCOMMAND | flags);
      }
      HWND frame = find_parent_hwnd(wi);
      if (frame) DrawMenuBar(frame);
    }
  }

  bool Session::tree_get_enabled(neui_widget_t widget, neui_item_t item)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return true;
    auto& w = _widgets[wi];

    if (is_treeview(w.type)) {
      auto it = w.tree_items.find(item.id);
      return (it != w.tree_items.end()) ? it->second.enabled : true;
    }
    if (is_menubar(w.type)) {
      auto it = w.menu_items.find(item.id);
      return (it != w.menu_items.end()) ? it->second.enabled : true;
    }
    return true;
  }

  void Session::tree_set_checked(neui_widget_t widget, neui_item_t item, bool checked)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return;
    auto& w = _widgets[wi];
    if (!is_menubar(w.type)) return;   // treeview ignores
    auto it = w.menu_items.find(item.id);
    // Leaf items only: a popup parent / separator can't carry a checkmark.
    if (it == w.menu_items.end() || it->second.is_separator || it->second.submenu) return;
    it->second.checked = checked;
    CheckMenuItem(it->second.parent_hmenu, it->second.cmd_id,
                  MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));
    HWND frame = find_parent_hwnd(wi);
    if (frame) DrawMenuBar(frame);
  }

  bool Session::tree_get_checked(neui_widget_t widget, neui_item_t item)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return false;
    auto& w = _widgets[wi];
    if (!is_menubar(w.type)) return false;
    auto it = w.menu_items.find(item.id);
    return (it != w.menu_items.end()) ? it->second.checked : false;
  }

  bool Session::try_translate_accel(MSG* msg)
  {
    if (!msg) return false;
    HWND root = msg->hwnd ? GetAncestor(msg->hwnd, GA_ROOT) : nullptr;
    if (!root) return false;
    for (uint32_t mb_idx : _menubars) {
      if (!_widgets.exists(mb_idx)) continue;
      auto& mb = _widgets[mb_idx];
      if (!mb.native_accel) continue;
      HWND frame = find_parent_hwnd(mb_idx);
      if (!frame || frame != root) continue;
      if (TranslateAcceleratorW(frame, mb.native_accel, msg)) return true;
    }
    return false;
  }

  // Rebuild the menubar's HACCEL from its current item shortcuts.
  static void rebuild_menubar_accel_w32(WidgetData& mb)
  {
    std::vector<neui_detail::AccelEntry> entries;
    entries.reserve(mb.menu_items.size());
    for (const auto& kv : mb.menu_items) {
      const auto& d = kv.second;
      if (d.submenu || d.is_separator) continue;
      if (d.shortcut_key != NEUI_KEY_NONE)
        entries.push_back({ d.shortcut_mods, d.shortcut_key, d.cmd_id });
      // Standard platform-alias shortcuts (e.g. Ctrl+Shift+Z for REDO).
      neui_detail::append_builtin_command_aliases(
        d.menu_cmd, d.shortcut_mods, d.shortcut_key, d.cmd_id, entries);
    }
    HACCEL old = mb.native_accel;
    mb.native_accel = neui_detail::build_accel_table(entries);
    if (old) DestroyAcceleratorTable(old);
  }

  void Session::tree_set_shortcut(neui_widget_t widget, neui_item_t item,
                                   uint32_t modifiers, uint32_t key)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return;
    auto& w = _widgets[wi];

    if (is_menubar(w.type)) {
      auto it = w.menu_items.find(item.id);
      if (it == w.menu_items.end()) return;
      it->second.shortcut_mods = modifiers;
      it->second.shortcut_key  = key;
      it->second.shortcut      =
        neui_detail::format_shortcut_label_win(modifiers, key);
      // Re-apply text with the new shortcut hint, then rebuild the accel table.
      tree_set_text(widget, item, it->second.text.c_str());
      rebuild_menubar_accel_w32(w);
    }
    // For treeview, shortcut hint is ignored.
  }

  neui_item_t Session::tree_get_first_child(neui_widget_t widget, neui_item_t parent)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return tree_item_none;
    auto& w = _widgets[wi];

    if (is_treeview(w.type) && w.hwnd) {
      HTREEITEM parent_hitem = (parent.id == 0) ? TVI_ROOT : nullptr;
      if (parent.id != 0) {
        auto it = w.tree_items.find(parent.id);
        if (it == w.tree_items.end()) return tree_item_none;
        parent_hitem = it->second.hitem;
      }
      HTREEITEM child = TreeView_GetChild(w.hwnd, parent_hitem);
      if (!child) return tree_item_none;
      auto it = w.tree_items_reverse.find(reinterpret_cast<uintptr_t>(child));
      return (it != w.tree_items_reverse.end()) ? neui_item_t{ it->second } : tree_item_none;
    }

    if (is_menubar(w.type)) {
      for (uint32_t id : w.menu_item_ids_ordered) {
        auto it = w.menu_items.find(id);
        if (it != w.menu_items.end() && it->second.parent_item_id == parent.id)
          return { id };
      }
    }
    return tree_item_none;
  }

  neui_item_t Session::tree_get_next_sibling(neui_widget_t widget, neui_item_t item)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return tree_item_none;
    auto& w = _widgets[wi];

    if (is_treeview(w.type) && w.hwnd) {
      auto it = w.tree_items.find(item.id);
      if (it == w.tree_items.end() || !it->second.hitem) return tree_item_none;
      HTREEITEM next = TreeView_GetNextSibling(w.hwnd, it->second.hitem);
      if (!next) return tree_item_none;
      auto rit = w.tree_items_reverse.find(reinterpret_cast<uintptr_t>(next));
      return (rit != w.tree_items_reverse.end()) ? neui_item_t{ rit->second } : tree_item_none;
    }

    if (is_menubar(w.type)) {
      auto cur = w.menu_items.find(item.id);
      if (cur == w.menu_items.end()) return tree_item_none;
      uint32_t parent_id = cur->second.parent_item_id;
      bool found = false;
      for (uint32_t id : w.menu_item_ids_ordered) {
        if (id == item.id) { found = true; continue; }
        if (!found) continue;
        auto it = w.menu_items.find(id);
        if (it != w.menu_items.end() && it->second.parent_item_id == parent_id)
          return { id };
      }
    }
    return tree_item_none;
  }

  neui_item_t Session::tree_get_selected(neui_widget_t widget)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return tree_item_none;
    auto& w = _widgets[wi];

    if (is_treeview(w.type) && w.hwnd) {
      HTREEITEM sel = TreeView_GetSelection(w.hwnd);
      if (!sel) return tree_item_none;
      auto it = w.tree_items_reverse.find(reinterpret_cast<uintptr_t>(sel));
      return (it != w.tree_items_reverse.end()) ? neui_item_t{ it->second } : tree_item_none;
    }
    return tree_item_none;  // menu bar: no persistent selection
  }

  void Session::tree_set_selected(neui_widget_t widget, neui_item_t item)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return;
    auto& w = _widgets[wi];

    if (is_treeview(w.type) && w.hwnd) {
      auto it = w.tree_items.find(item.id);
      if (it != w.tree_items.end() && it->second.hitem)
        TreeView_SelectItem(w.hwnd, it->second.hitem);
    }
    // menu bar: no-op
  }

  // ---------------------------------------------------------------------------
  // Tree API static wrappers
  // ---------------------------------------------------------------------------

  static neui_item_t tree_add_fn(neui_session_t session, neui_widget_t widget,
                                  neui_item_t parent, const char* text, void* userdata)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->tree_add(widget, parent, text, userdata) : tree_item_none;
  }
  static void tree_remove_fn(neui_session_t session, neui_widget_t widget, neui_item_t item)
  {
    auto s = get_session_for_widget(session, widget); if (s) s->tree_remove(widget, item);
  }
  static void tree_clear_fn(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget); if (s) s->tree_clear(widget);
  }
  static int tree_get_text_fn(neui_session_t session, neui_widget_t widget, neui_item_t item, char* buf, int buflen)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->tree_get_text(widget, item, buf, buflen) : -1;
  }
  static void tree_set_text_fn(neui_session_t session, neui_widget_t widget, neui_item_t item, const char* text)
  {
    auto s = get_session_for_widget(session, widget); if (s) s->tree_set_text(widget, item, text);
  }
  static void* tree_get_userdata_fn(neui_session_t session, neui_widget_t widget, neui_item_t item)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->tree_get_userdata(widget, item) : nullptr;
  }
  static void tree_set_enabled_fn(neui_session_t session, neui_widget_t widget, neui_item_t item, bool enabled)
  {
    auto s = get_session_for_widget(session, widget); if (s) s->tree_set_enabled(widget, item, enabled);
  }
  static bool tree_get_enabled_fn(neui_session_t session, neui_widget_t widget, neui_item_t item)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->tree_get_enabled(widget, item) : true;
  }
  static void tree_set_shortcut_fn(neui_session_t session, neui_widget_t widget, neui_item_t item,
                                    uint32_t modifiers, uint32_t key)
  {
    auto s = get_session_for_widget(session, widget); if (s) s->tree_set_shortcut(widget, item, modifiers, key);
  }
  static neui_item_t tree_get_first_child_fn(neui_session_t session, neui_widget_t widget, neui_item_t parent)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->tree_get_first_child(widget, parent) : tree_item_none;
  }
  static neui_item_t tree_get_next_sibling_fn(neui_session_t session, neui_widget_t widget, neui_item_t item)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->tree_get_next_sibling(widget, item) : tree_item_none;
  }
  static neui_item_t tree_get_selected_fn(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->tree_get_selected(widget) : tree_item_none;
  }
  static void tree_set_selected_fn(neui_session_t session, neui_widget_t widget, neui_item_t item)
  {
    auto s = get_session_for_widget(session, widget); if (s) s->tree_set_selected(widget, item);
  }
  static void tree_set_menu_cmd_fn(neui_session_t session, neui_widget_t widget,
                                    neui_item_t item, uint32_t command)
  {
    auto s = get_session_for_widget(session, widget); if (!s) return;
    uint32_t wi = WidgetToIndex(widget);
    auto* w = s->get_widget(wi);
    if (!w || !is_menubar(w->type)) return;
    auto it = w->menu_items.find(item.id);
    if (it != w->menu_items.end()) it->second.menu_cmd = command;
  }
  static void tree_set_checked_fn(neui_session_t session, neui_widget_t widget, neui_item_t item, bool checked)
  {
    auto s = get_session_for_widget(session, widget); if (s) s->tree_set_checked(widget, item, checked);
  }
  static bool tree_get_checked_fn(neui_session_t session, neui_widget_t widget, neui_item_t item)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->tree_get_checked(widget, item) : false;
  }

  neui_tree_api_t tree_api = {
    tree_add_fn,
    tree_remove_fn,
    tree_clear_fn,
    tree_get_text_fn,
    tree_set_text_fn,
    tree_get_userdata_fn,
    tree_set_enabled_fn,
    tree_get_enabled_fn,
    tree_set_shortcut_fn,
    tree_get_first_child_fn,
    tree_get_next_sibling_fn,
    tree_get_selected_fn,
    tree_set_selected_fn,
    tree_set_menu_cmd_fn,
    tree_set_checked_fn,
    tree_get_checked_fn,
  };

  static void set_focus(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) s->widget_set_focus(widget);
  }

  static bool is_checkbox(const char* type) {
    return type && (!strcmp(type, NEUI_W_CHECKBOX) || !strcmp(type, NEUI_W_CHECKBOX3));
  }

  void Session::widget_set_check(neui_widget_t widget, neui_check_state_t state)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return;
    auto& w = _widgets[index];
    if (!is_checkbox(w.type)) return;
    if (!w.hwnd) {
      w.pending_check = (int)state;
      return;
    }
    SendMessageW(w.hwnd, BM_SETCHECK, (WPARAM)state, 0);
  }

  neui_check_state_t Session::widget_get_check(neui_widget_t widget)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return NEUI_CHECK_UNCHECKED;
    auto& w = _widgets[index];
    if (!is_checkbox(w.type) || !w.hwnd) return NEUI_CHECK_UNCHECKED;
    LRESULT state = SendMessageW(w.hwnd, BM_GETCHECK, 0, 0);
    if (state == BST_CHECKED)       return NEUI_CHECK_CHECKED;
    if (state == BST_INDETERMINATE) return NEUI_CHECK_INDETERMINATE;
    return NEUI_CHECK_UNCHECKED;
  }

  static void set_check(neui_session_t session, neui_widget_t widget, neui_check_state_t state)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) s->widget_set_check(widget, state);
  }

  static neui_check_state_t get_check(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget);
    return s ? s->widget_get_check(widget) : NEUI_CHECK_UNCHECKED;
  }

  void* Session::widget_get_native_handle(neui_widget_t widget)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return nullptr;
    return static_cast<void*>(_widgets[index].hwnd);
  }

  static void* get_native_handle(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) return s->widget_get_native_handle(widget);
    return nullptr;
  }

  // Bind an asset handle as the IMAGE widget's source. Releases the
  // widget's internally-owned slot (allocated by widget_set_text) if
  // any, then stores the client-supplied handle so paint_image_w32
  // resolves it on the next frame. The per-ctx GPU upload happens
  // lazily on first paint and is cleaned up by PaintedWndProc's
  // WM_DESTROY via _asset_manager.release_context.
  void Session::widget_set_asset(neui_widget_t widget, neui_asset_t asset)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return;
    auto& w = _widgets[index];
    if (!w.type) return;

    // Reject cross-session handles before touching state - keeps a
    // rejected call a true no-op. asset_none always passes (documented
    // clear).
    if (asset.id != asset_none.id &&
        ((asset.id >> 16) & 0xffff) != (_session_id & 0xffff)) {
      return;
    }

    if (strcmp(w.type, NEUI_W_IMAGE) == 0) {
      // Release the previous internal slot (only ours to free; client-
      // supplied handles stay owned by the client).
      if (w.image_asset_owned && w.image_asset.id != asset_none.id) {
        _asset_manager.release_slot(w.image_asset.id & 0xffff,
                                     neui_d2d_backend::get_backend());
      }
      w.image_asset       = asset;
      w.image_asset_owned = false;
      w.text.clear();  // mutual-clear: asset path is live, drop the path
    }
    else if (strcmp(w.type, NEUI_W_CUSTOMDRAW) == 0) {
      // CUSTOMDRAW + compound: compound replaces WIDGET_PAINT dispatch.
      // CUSTOMDRAW + behavior: behavior routes input events to attr writes.
      // Kind-route by inspecting the asset entry; asset_none clears the
      // compound slot (matches the v1 contract for IMAGE / CUSTOMDRAW).
      if (asset.id == asset_none.id) {
        w.compound_asset = asset_none;
      } else {
        auto* entry = _asset_manager.get_slot(asset.id & 0xffff);
        if (entry && entry->kind == NEUI_ASSET_KIND_BEHAVIOR) {
          w.behavior_asset = asset;
        } else if (entry && entry->kind == NEUI_ASSET_KIND_COMPONENT) {
          // COMPONENT bundles compound + behavior + defaults: attach both
          // slots and stamp defaults (keys the widget lacks; client pre-sets win).
          w.compound_asset = entry->comp_compound;
          w.behavior_asset = entry->comp_behavior;
          auto& bag = neui_detail::ensure_attrs(w.attrs);
          for (const auto& d : entry->comp_defaults) {
            if (bag.has(d.key)) continue;
            switch (d.type) {
              case neui_detail::ComponentDefaultAttr::INT:    bag.set_int(d.key, d.ival); break;
              case neui_detail::ComponentDefaultAttr::FLOAT:  bag.set_float(d.key, d.fval); break;
              case neui_detail::ComponentDefaultAttr::STRING: bag.set_string(d.key, d.sval.c_str()); break;
            }
          }
        } else {
          w.compound_asset = asset;
        }
      }
    }
    else {
      return;  // unsupported widget type
    }

    if (w.hwnd) InvalidateRect(w.hwnd, nullptr, FALSE);
  }

  static void NEUI_ABI set_asset(neui_session_t session, neui_widget_t widget, neui_asset_t asset)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) s->widget_set_asset(widget, asset);
  }

  static void NEUI_ABI set_enabled(neui_session_t session, neui_widget_t widget, bool enabled)
  {
    auto s = get_session_for_widget(session, widget);
    if (!s) return;
    auto* wd = s->get_widget(WidgetToIndex(widget));
    if (!wd) return;
    if (wd->enabled == enabled) return;
    wd->enabled = enabled;
    // Push the flag into the live HWND. If the HWND has not been created
    // yet (deferred), create_child_windows will apply wd->enabled at HWND
    // creation time.
    if (wd->hwnd)
      EnableWindow(wd->hwnd, enabled ? TRUE : FALSE);
  }

  static bool NEUI_ABI get_enabled(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget);
    if (!s) return false;
    auto* wd = s->get_widget(WidgetToIndex(widget));
    if (!wd) return false;
    return wd->enabled;
  }

  // Native Win32 controls carry WS_TABSTOP at creation and native
  // WM_GETDLGCODE / IsDialogMessage handles traversal, so this is a no-op.
  // The entry exists to keep the vtable layout in sync with neui_widget_api_t.
  static void set_tab_stop(neui_session_t /*session*/, neui_widget_t /*widget*/, bool /*enabled*/)
  {
  }

  static void set_owner(neui_session_t session, neui_widget_t dialog, neui_widget_t owner)
  {
    auto s = get_session_for_widget(session, dialog);
    if (!s) return;
    if (!widget_belongs_to_session(owner, s->session_id())) return;
    s->widget_set_owner(dialog, owner);
  }

  static void get_pos(neui_session_t session, neui_widget_t widget,
                      int* x, int* y)
  {
    auto s = get_session_for_widget(session, widget);
    if (!s) return;
    auto* wd = s->get_widget(widget.id);
    if (!wd) return;
    if (x) *x = wd->x;
    if (y) *y = wd->y;
  }

  static void get_size(neui_session_t session, neui_widget_t widget,
                       int* width, int* height)
  {
    auto s = get_session_for_widget(session, widget);
    if (!s) return;
    auto* wd = s->get_widget(widget.id);
    if (!wd) return;
    if (width)  *width  = wd->width;
    if (height) *height = wd->height;
  }

  // Content area of a widget in its own coordinate space. Win32 frames carry a
  // native HMENU (non-client), so the client area is the full widget rect with
  // a (0, 0) origin - no in-frame band inset like the Linux xpl host.
  static void get_client_rect_api(neui_session_t session, neui_widget_t widget,
                                  int* x, int* y, int* width, int* height)
  {
    auto s = get_session_for_widget(session, widget);
    if (!s) return;
    auto* wd = s->get_widget(WidgetToIndex(widget));
    if (!wd) return;
    if (x)      *x      = 0;
    if (y)      *y      = 0;
    if (width)  *width  = wd->width;
    if (height) *height = wd->height;
  }

  // ---------------------------------------------------------------------------
  // Invalidate (request repaint)

  // Request a repaint of `widget`. For HWND-backed widgets this is
  // InvalidateRect(NULL) which coalesces with any other pending paints
  // until the next WM_PAINT. No-op when the HWND doesn't exist yet
  // (deferred-HWND path - the widget will paint correctly on first show).
  static void NEUI_ABI invalidate(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget);
    if (!s) return;
    auto* wd = s->get_widget(WidgetToIndex(widget));
    if (!wd || !wd->hwnd) return;
    InvalidateRect(wd->hwnd, nullptr, FALSE);
  }

  // ---------------------------------------------------------------------------
  // Popup menu (context menu, free of the menubar)

  static int NEUI_ABI popup_menu(neui_session_t session, neui_widget_t anchor,
                                  int x, int y, const char* const* items)
  {
    auto* s = get_session(session);
    if (!s) return 0;
    auto* wd = s->get_widget(WidgetToIndex(anchor));
    if (!wd || !wd->session) return 0;
    if (!items) return 0;

    // Find the parent frame's HWND for screen-coord conversion + as the
    // owner of TrackPopupMenuEx.
    HWND owner = wd->session->find_parent_hwnd(wd->index);
    if (!owner) owner = wd->hwnd;
    if (!owner) return 0;

    HMENU h = CreatePopupMenu();
    if (!h) return 0;

    int count = 0;
    for (int i = 0; items[i] != nullptr; ++i) {
      const char* s8 = items[i];
      bool is_sep = (s8[0] == '-' && s8[1] == 0);
      ++count;
      if (is_sep) {
        AppendMenuW(h, MF_SEPARATOR, 0, nullptr);
      } else {
        // Convert UTF-8 to UTF-16.
        int wlen = MultiByteToWideChar(CP_UTF8, 0, s8, -1, nullptr, 0);
        std::wstring ws(wlen ? wlen - 1 : 0, L'\0');
        if (wlen > 1) {
          MultiByteToWideChar(CP_UTF8, 0, s8, -1, ws.data(), wlen);
        }
        AppendMenuW(h, MF_STRING, static_cast<UINT_PTR>(count), ws.c_str());
      }
    }

    // Convert (x, y) from anchor-local logical to screen physical.
    UINT dpi = wd->session->get_dpi_for_widget(wd->index);
    if (dpi == 0) dpi = 96;
    POINT pt;
    pt.x = LogicalToPhysical(wd->x + x, dpi);
    pt.y = LogicalToPhysical(wd->y + y, dpi);
    // Walk up to the frame: anchor's (x, y) above are relative to its
    // immediate parent. find_parent_hwnd returned the frame; we need the
    // anchor's parent HWND to map correctly. For knob widgets the parent
    // is the frame, so this works. For deeper nesting we'd need to walk.
    if (wd->hwnd) {
      // anchor itself has an HWND - pt(x,y) is in its client coords.
      pt.x = LogicalToPhysical(x, dpi);
      pt.y = LogicalToPhysical(y, dpi);
      ClientToScreen(wd->hwnd, &pt);
    } else {
      ClientToScreen(owner, &pt);
    }

    UINT picked = TrackPopupMenuEx(h,
        TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN,
        pt.x, pt.y, owner, nullptr);
    DestroyMenu(h);
    return static_cast<int>(picked);
  }

  // -------------------------------------------------------------------------
  // Notify API (NEUI_API_NOTIFY) - toast + message box. Host-owned chrome
  // anchored to a frame, outside the widget tree.

  // Resolve `parent_window` to a top-level frame's HWND (APPWINDOW /
  // PLUGWINDOW / DIALOG); nullptr for anything else.
  static HWND notify_frame_hwnd(neui_session_t session, neui_widget_t parent_window)
  {
    auto* s = get_session_for_widget(session, parent_window);
    if (!s) return nullptr;
    uint32_t idx = WidgetToIndex(parent_window);
    auto* wd = s->get_widget(idx);
    if (!wd || !wd->hwnd || !wd->type) return nullptr;
    bool ok = wd->isroot
              || !strcmp(wd->type, NEUI_W_PLUGWINDOW)
              || !strcmp(wd->type, NEUI_W_DIALOG);
    return ok ? wd->hwnd : nullptr;
  }

  static void NEUI_ABI notify_toast(neui_session_t session,
                                     neui_widget_t parent_window,
                                     const char* text)
  {
    HWND hwnd = notify_frame_hwnd(session, parent_window);
    if (!hwnd) return;
    neui_detail::toast_show_w32(hwnd, text ? text : "");
  }

  // NEUI_MB_* values match MB_* numerically, so the flags pass through to
  // MessageBoxExW after a sanitize mask. The OS handles owner disabling,
  // focus return, and Esc/Enter.
  static int NEUI_ABI notify_message_box(neui_session_t session,
                                          neui_widget_t parent_window,
                                          const char* text, const char* caption,
                                          uint32_t flags)
  {
    HWND hwnd = notify_frame_hwnd(session, parent_window);
    if (!hwnd) return 0;
    UINT mb = flags & (MB_TYPEMASK | MB_ICONMASK | MB_DEFMASK);
    if ((mb & MB_TYPEMASK) > MB_CANCELTRYCONTINUE) mb &= ~MB_TYPEMASK;
    std::wstring wtext    = ToWide(text);
    std::wstring wcaption = ToWide(caption);
    return MessageBoxExW(hwnd, wtext.c_str(),
                         caption ? wcaption.c_str() : nullptr,
                         mb, MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL));
  }

  // open_file / save_file via IFileDialog (shared helper). The owner HWND is
  // what makes the dialog modal, so an unrealised frame cannot host one; -1
  // reports that as "no dialog", which the contract keeps distinct from 0 =
  // cancelled.
  static int notify_run_file_dialog(neui_session_t session,
                                    neui_widget_t parent_window,
                                    const neui_file_dialog_t* desc,
                                    neui_file_path_cb cb, void* userdata,
                                    bool save)
  {
    HWND hwnd = notify_frame_hwnd(session, parent_window);
    if (!hwnd) return -1;
    neui_file_dialog_t empty = {};
    if (!desc) desc = &empty;

    std::vector<std::string> paths;
    int n = save ? neui_detail::file_dialog_save_win32(hwnd, desc, paths)
                 : neui_detail::file_dialog_open_win32(hwnd, desc, paths);
    if (n <= 0) return n;
    if (cb)
      for (const auto& p : paths) cb(userdata, p.c_str());
    return static_cast<int>(paths.size());
  }

  static int NEUI_ABI notify_open_file(neui_session_t session,
                                        neui_widget_t parent_window,
                                        const neui_file_dialog_t* desc,
                                        neui_file_path_cb cb, void* userdata)
  {
    return notify_run_file_dialog(session, parent_window, desc, cb, userdata, false);
  }

  static int NEUI_ABI notify_save_file(neui_session_t session,
                                        neui_widget_t parent_window,
                                        const neui_file_dialog_t* desc,
                                        neui_file_path_cb cb, void* userdata)
  {
    return notify_run_file_dialog(session, parent_window, desc, cb, userdata, true);
  }

  neui_notify_api_t notify_api = {
    NEUI_VERSION,
    notify_toast,
    notify_message_box,
    notify_open_file,
    notify_save_file,
  };

  neui_widget_api_t widgets_api = {
    create,
    destroy,
    show,
    hide,
    set_pos,
    set_size,
    set_emit_events,
    set_text,
    get_text,
    get_first_child,
    get_next_sibling,
    set_focus,
    set_check,
    get_check,
    get_native_handle,
    set_tab_stop,
    set_owner,
    get_pos,
    get_size,
    popup_menu,
    invalidate,
    set_asset,
    set_enabled,
    get_enabled,
    get_client_rect_api,
    create_from_component,
    // popup_tree_menu: not implemented on the native win32 host. Would need
    // TrackPopupMenuEx on an HMENU built from the POPUPMENU's item model
    // (which this host does not keep - its menus live in HMENUs directly).
    // Explicit nullptr so this reads as a decision, not a missed initializer;
    // clients null-check appended slots per <neui/d/widgets.h>. Use the xpl
    // host (neui.host.crossplatform) for a tree-model context menu.
    nullptr,
  };

  // -------------------------------------------------------------------------
  // Attribute API

  namespace {
    WidgetData* get_widget_ptr(neui_session_t session, neui_widget_t widget) {
      auto* s = get_session(session);
      if (!s) return nullptr;
      return s->get_widget(WidgetToIndex(widget));
    }
  }

  static int NEUI_ABI a_set_int(neui_session_t session, neui_widget_t widget,
                                 const char* key, int32_t value)
  {
    auto* w = get_widget_ptr(session, widget);
    if (!w || !key) return 0;
    neui_detail::ensure_attrs(w->attrs).set_int(key, value);

    // Live re-application for paint-affecting int attrs on self-painted
    // widgets. NEUI_ATTR_BACKGROUND drives body fill (SECTION), the
    // begin_frame clear (KNOB / IMAGE), so a runtime change needs a
    // repaint. Region geometry doesn't depend on colour, so SECTION can
    // skip apply_section_region_w32 here.
    if (w->hwnd && w->type && !strcmp(key, NEUI_ATTR_BACKGROUND) &&
        (!strcmp(w->type, NEUI_W_SECTION)    ||
         !strcmp(w->type, NEUI_W_KNOB)       ||
         !strcmp(w->type, NEUI_W_IMAGE)      ||
         !strcmp(w->type, NEUI_W_CUSTOMDRAW))) {
      InvalidateRect(w->hwnd, nullptr, FALSE);
    }
    // COMBOBOX drop-list geometry attrs: re-size the native dropdown live.
    if (w->hwnd && w->type && !strcmp(w->type, NEUI_W_COMBOBOX) &&
        (!strcmp(key, NEUI_ATTR_COMBO_MAX_VISIBLE) ||
         !strcmp(key, NEUI_ATTR_COMBO_DROP_WIDTH))) {
      apply_combo_drop_sizing_w32(*w);
    }
    // Font weight: rebuild the per-widget HFONT and re-broadcast via
    // WM_SETFONT. Native controls (Edit, Button, Static, Checkbox) pick
    // up the new font immediately; painted widgets re-read attrs on the
    // next paint, so an extra InvalidateRect is required.
    if (w->hwnd && !strcmp(key, NEUI_ATTR_FONT_WEIGHT)) {
      ensure_custom_font_w32(*w);
      InvalidateRect(w->hwnd, nullptr, FALSE);
    }
    // CUSTOMDRAW + compound: any attr change can drive a layer binding
    // or a template substitution, so a compound-attached widget repaints
    // on every attr touch. Cheap: InvalidateRect is idempotent.
    if (w->hwnd && w->type && !strcmp(w->type, NEUI_W_CUSTOMDRAW) &&
        w->compound_asset.id != asset_none.id) {
      InvalidateRect(w->hwnd, nullptr, FALSE);
    }
    // SECTION / TABPAGE content extent override: recompute layout +
    // reposition children so the scrollbar reflects the new content size.
    if (is_section_like_w32(w->type) &&
        (!strcmp(key, NEUI_ATTR_CONTENT_WIDTH) ||
         !strcmp(key, NEUI_ATTR_CONTENT_HEIGHT))) {
      section_apply_layout_changes_w32(*w);
    }
    // TABPAGE chip colours / background -> repaint the page + re-flow the
    // parent TABVIEW strip (the active page's bg drives the tabview body
    // fill + active-chip colour).
    if (w->type && !strcmp(w->type, NEUI_W_TABPAGE) &&
        (!strcmp(key, NEUI_ATTR_TAB_CHIP_BG_COLOR) ||
         !strcmp(key, NEUI_ATTR_TAB_CHIP_TEXT_COLOR) ||
         !strcmp(key, NEUI_ATTR_BACKGROUND))) {
      if (w->hwnd) InvalidateRect(w->hwnd, nullptr, FALSE);
      if (auto* tv = tabview_parent_of_page_w32(*w)) tabview_relayout_w32(*tv);
    }
    // TABVIEW style attrs -> re-flow the strip + repaint.
    if (w->hwnd && w->type && !strcmp(w->type, NEUI_W_TABVIEW) &&
        (!strcmp(key, NEUI_ATTR_TAB_STRIP_SIZE)     ||
         !strcmp(key, NEUI_ATTR_TAB_BORDER_COLOR)   ||
         !strcmp(key, NEUI_ATTR_TAB_BORDER_WIDTH)   ||
         !strcmp(key, NEUI_ATTR_TAB_CHIP_RADIUS)    ||
         !strcmp(key, NEUI_ATTR_TAB_STRIP_BG_COLOR) ||
         !strcmp(key, NEUI_ATTR_BACKGROUND))) {
      tabview_relayout_w32(*w);
    }
    return 1;
  }

  static int32_t NEUI_ABI a_get_int(neui_session_t session, neui_widget_t widget,
                                     const char* key, int32_t default_value)
  {
    auto* w = get_widget_ptr(session, widget);
    if (!w || !key || !w->attrs) return default_value;
    return w->attrs->get_int(key, default_value);
  }

  static int NEUI_ABI a_set_string(neui_session_t session, neui_widget_t widget,
                                    const char* key, const char* value)
  {
    auto* w = get_widget_ptr(session, widget);
    if (!w || !key) return 0;
    neui_detail::ensure_attrs(w->attrs).set_string(key, value);

    // Live re-application for behavior-bearing keys.
    if (w->isroot && w->hwnd && !strcmp(key, NEUI_ATTR_ICON_PATH)) {
      neui_detail::apply_window_icon(w->hwnd, value, &w->native_icon);
    }
    // Font family: rebuild custom HFONT (native controls update via
    // WM_SETFONT; painted widgets pick it up on the next paint).
    if (w->hwnd && !strcmp(key, NEUI_ATTR_FONT_FAMILY)) {
      ensure_custom_font_w32(*w);
      InvalidateRect(w->hwnd, nullptr, FALSE);
    }
    // Section: align change moves the title chip, so the window region
    // has to be rebuilt before the next paint. Also covers background
    // changes via the InvalidateRect (region geometry doesn't depend on
    // colour, but a repaint is needed to pick up the new fill).
    if (w->hwnd && w->type && !strcmp(w->type, NEUI_W_SECTION) &&
        !strcmp(key, NEUI_ATTR_ALIGN_TEXT)) {
      apply_section_region_w32(*w);
      // align="none" / non-empty band swap changes band_h -> layout.
      section_apply_layout_changes_w32(*w);
    }
    // Section / TABPAGE scroll-mode change: allocate / drop scroll state,
    // switch the painted_msg_fn hook, and reset the scroll offset. A TABPAGE
    // is a chip-less section and opts into scrolling the same way.
    if (w->hwnd && is_section_like_w32(w->type) &&
        !strcmp(key, NEUI_ATTR_SCROLL_MODE)) {
      section_refresh_scroll_state_w32(*w);
      bool now_scrolling = (w->section_scroll_state != nullptr);
      // The inner body HWND is created the first time a section becomes
      // scrollable and KEPT for its lifetime - including after switching
      // back to "none" - so children stay body-local (laid out below the
      // chip band) + clipped to the body rect in every mode. Destroying it
      // on a flip-to-"none" dropped children to section-local coords, so
      // they jumped up into the chip band and overpainted it. Mirror of the
      // macOS host. Children only need re-parenting the first time the body
      // HWND appears.
      if (now_scrolling && !w->section_body_hwnd) {
        LONG style = GetWindowLongW(w->hwnd, GWL_STYLE);
        if (!(style & WS_CLIPCHILDREN))
          SetWindowLongW(w->hwnd, GWL_STYLE, style | WS_CLIPCHILDREN);
        section_create_body_hwnd_w32(*w);
        section_reparent_children_w32(*w, /*to_body*/true);
      }
      // Toggle wheel / scrollbar input handling with the mode; the body HWND
      // itself stays put.
      w->painted_msg_fn = now_scrolling ? &painted_msg_section_w32 : nullptr;
      w->emit_events    = now_scrolling;
      if (!now_scrolling)
        KillTimer(w->hwnd, SECTION_BOUNCE_TIMER_ID);
      // Switching scroll mode resets the scroll offset to 0,0. (For "none"
      // the scroll state is gone and the reposition below already uses
      // offset 0; this covers scroll->scroll transitions like vertical->both.)
      if (w->section_scroll_state) {
        auto& st = *w->section_scroll_state;
        st.scroll_x = st.scroll_y = 0;
        st.kin_v = st.kin_h = neui_detail::ScrollKinetics{};
        st.kinetic_over_v = st.kinetic_over_h = false;
      }
      section_apply_layout_changes_w32(*w);
    }
    // TABVIEW: NEUI_ATTR_TAB_POSITION changes the strip edge + content body
    // rect, so re-flow the pages + repaint the strip.
    if (w->hwnd && w->type && !strcmp(w->type, NEUI_W_TABVIEW) &&
        !strcmp(key, NEUI_ATTR_TAB_POSITION)) {
      tabview_relayout_w32(*w);
    }
    // CUSTOMDRAW + compound: any attr change can change a text-layer's
    // template resolution, so repaint.
    if (w->hwnd && w->type && !strcmp(w->type, NEUI_W_CUSTOMDRAW) &&
        w->compound_asset.id != asset_none.id) {
      InvalidateRect(w->hwnd, nullptr, FALSE);
    }
    return 1;
  }

  static const char* NEUI_ABI a_get_string(neui_session_t session,
                                            neui_widget_t widget, const char* key)
  {
    auto* w = get_widget_ptr(session, widget);
    if (!w || !key || !w->attrs) return nullptr;
    return w->attrs->get_string(key);
  }

  static int NEUI_ABI a_has(neui_session_t session, neui_widget_t widget,
                             const char* key)
  {
    auto* w = get_widget_ptr(session, widget);
    if (!w || !key) return 0;
    return (w->attrs && w->attrs->has(key)) ? 1 : 0;
  }

  static int NEUI_ABI a_remove(neui_session_t session, neui_widget_t widget,
                                const char* key)
  {
    auto* w = get_widget_ptr(session, widget);
    if (!w || !key || !w->attrs) return 0;
    bool removed = w->attrs->remove(key);
    if (removed && w->hwnd &&
        (!strcmp(key, NEUI_ATTR_FONT_FAMILY) ||
         !strcmp(key, NEUI_ATTR_FONT_SIZE)   ||
         !strcmp(key, NEUI_ATTR_FONT_WEIGHT))) {
      ensure_custom_font_w32(*w);
      InvalidateRect(w->hwnd, nullptr, FALSE);
    }
    // CUSTOMDRAW + compound: removing a key can change a layer binding or a
    // template substitution (e.g. clearing a bound {token} or a value), so
    // repaint - matching a_set_int / a_set_float / a_set_string. Without
    // this the compound shows stale pixels until an unrelated repaint.
    if (removed && w->hwnd && w->type && !strcmp(w->type, NEUI_W_CUSTOMDRAW) &&
        w->compound_asset.id != asset_none.id) {
      InvalidateRect(w->hwnd, nullptr, FALSE);
    }
    return removed ? 1 : 0;
  }

  static int NEUI_ABI a_set_float(neui_session_t session, neui_widget_t widget,
                                   const char* key, float value)
  {
    auto* w = get_widget_ptr(session, widget);
    if (!w || !key) return 0;

    // Clamp + (when steps is set) snap NEUI_PARAM_VALUE on entry so
    // attribute storage and anything reading it back (paint, native control
    // state) stay in sync.
    float stored = value;
    if (!strcmp(key, NEUI_PARAM_VALUE)) {
      stored = clamp01_w32(value);
      stored = snap_to_steps_w32(stored, widget_get_steps_w32(*w));
    }
    neui_detail::ensure_attrs(w->attrs).set_float(key, stored);

    // Live re-application for value-bearing widgets so programmatic sets
    // immediately reflect in the visible state. Programmatic set is
    // intentionally silent - no NEUI_EVENT_VALUE_CHANGED.
    if (w->hwnd && w->type && !strcmp(key, NEUI_PARAM_VALUE)) {
      if (!strcmp(w->type, NEUI_W_SLIDER)) {
        DWORD style = static_cast<DWORD>(GetWindowLongW(w->hwnd, GWL_STYLE));
        bool vertical = (style & TBS_VERT) != 0;
        int  pos = static_cast<int>((vertical ? (1.0f - stored) : stored) * 1000.0f + 0.5f);
        SendMessageW(w->hwnd, TBM_SETPOS, TRUE, pos);
      } else if (!strcmp(w->type, NEUI_W_KNOB)) {
        InvalidateRect(w->hwnd, nullptr, FALSE);
      }
    }

    // NEUI_ATTR_ROTATION on IMAGE: invalidate so the bitmap repaints with
    // the new transform on the next paint pass. Native controls don't
    // need this; only painted widgets read the attr in their draw path.
    if (w->hwnd && w->type && !strcmp(key, NEUI_ATTR_ROTATION) &&
        !strcmp(w->type, NEUI_W_IMAGE)) {
      InvalidateRect(w->hwnd, nullptr, FALSE);
    }
    // Font size: rebuild HFONT for native controls; painted widgets pick
    // it up on the next paint.
    if (w->hwnd && !strcmp(key, NEUI_ATTR_FONT_SIZE)) {
      ensure_custom_font_w32(*w);
      InvalidateRect(w->hwnd, nullptr, FALSE);
    }
    // CUSTOMDRAW + compound: any float attr change can drive a binding,
    // so repaint.
    if (w->hwnd && w->type && !strcmp(w->type, NEUI_W_CUSTOMDRAW) &&
        w->compound_asset.id != asset_none.id) {
      InvalidateRect(w->hwnd, nullptr, FALSE);
    }
    return 1;
  }

  static float NEUI_ABI a_get_float(neui_session_t session, neui_widget_t widget,
                                     const char* key, float default_value)
  {
    auto* w = get_widget_ptr(session, widget);
    if (!w || !key || !w->attrs) return default_value;
    return w->attrs->get_float(key, default_value);
  }

  static int NEUI_ABI a_set_session_int(neui_session_t session,
                                         const char* key, int32_t value)
  {
    auto* s = get_session(session);
    if (!s || !key) return 0;
    s->_session_attrs.set_int(key, value);
    // Live-apply the keys we know about. NEUI_ATTR_THEME_MODE is the
    // only session-level key with behaviour today; recompute the
    // effective palette and re-paint as if the system theme had flipped.
    if (!strcmp(key, NEUI_ATTR_THEME_MODE)) {
      s->on_theme_changed();
    }
    return 1;
  }

  static int32_t NEUI_ABI a_get_session_int(neui_session_t session,
                                             const char* key,
                                             int32_t default_value)
  {
    auto* s = get_session(session);
    if (!s || !key) return default_value;
    return s->_session_attrs.get_int(key, default_value);
  }

  neui_attr_api_t attrs_api = {
    NEUI_VERSION,
    a_set_int,
    a_get_int,
    a_set_string,
    a_get_string,
    a_has,
    a_remove,
    a_set_float,
    a_get_float,
    a_set_session_int,
    a_get_session_int,
  };

  // -------------------------------------------------------------------------
  // Clipboard API

  static int NEUI_ABI cb_set_text(neui_session_t /*session*/, const char* utf8)
  {
    if (!utf8) return 0;
    return neui_detail::clipboard_set_text_win32(
             utf8, static_cast<uint32_t>(strlen(utf8))) ? 1 : 0;
  }

  static int NEUI_ABI cb_get_text(neui_session_t /*session*/, char* buf, int buflen)
  {
    return neui_detail::clipboard_get_text_win32(buf, buflen);
  }

  static bool NEUI_ABI cb_has_text(neui_session_t /*session*/)
  {
    return neui_detail::clipboard_has_text_win32();
  }

  static neui_data_item_t NEUI_ABI cb_read(neui_session_t session)
  {
    auto* s = get_session(session);
    if (!s) return neui_data_item_none;
    uint32_t id = s->_data_items.allocate();
    auto* item = s->_data_items.get(id);
    if (!item) return neui_data_item_none;
    if (!neui_detail::clipboard_read_item_win32(*item)) {
      s->_data_items.release(id);
      return neui_data_item_none;
    }
    return { id };
  }

  static neui_data_item_t NEUI_ABI cb_create_item(neui_session_t session)
  {
    auto* s = get_session(session);
    if (!s) return neui_data_item_none;
    return { s->_data_items.allocate() };
  }

  static void NEUI_ABI cb_release(neui_session_t session,
                                   neui_data_item_t item)
  {
    auto* s = get_session(session);
    if (!s) return;
    s->_data_items.release(item.id);
  }

  static int NEUI_ABI cb_write(neui_session_t session,
                                neui_data_item_t item)
  {
    auto* s = get_session(session);
    if (!s) return 0;
    auto* it = s->_data_items.get(item.id);
    if (!it) return 0;
    return neui_detail::clipboard_write_item_win32(*it) ? 1 : 0;
  }

  static int NEUI_ABI cb_item_set_format(neui_session_t session,
                                          neui_data_item_t item,
                                          const char* mime,
                                          const void* data, uint32_t length)
  {
    auto* s = get_session(session);
    if (!s || !mime) return 0;
    auto* it = s->_data_items.get(item.id);
    if (!it) return 0;
    it->set_format(mime, data, length);
    return 1;
  }

  static int NEUI_ABI cb_item_get_format(neui_session_t session,
                                          neui_data_item_t item,
                                          const char* mime,
                                          void* buf, int buflen)
  {
    auto* s = get_session(session);
    if (!s || !mime) return -1;
    auto* it = s->_data_items.get(item.id);
    if (!it) return -1;
    return it->get_format(mime, buf, buflen);
  }

  static bool NEUI_ABI cb_item_has_format(neui_session_t session,
                                           neui_data_item_t item,
                                           const char* mime)
  {
    auto* s = get_session(session);
    if (!s || !mime) return false;
    auto* it = s->_data_items.get(item.id);
    return it && it->has_format(mime);
  }

  static int NEUI_ABI cb_item_set_format_callback(neui_session_t session,
                                                   neui_data_item_t item,
                                                   const char* mime,
                                                   neui_data_provider_t provider,
                                                   void* userdata)
  {
    auto* s = get_session(session);
    if (!s || !mime || !provider) return 0;
    auto* it = s->_data_items.get(item.id);
    if (!it) return 0;
    it->set_format_provider(mime, provider, userdata);
    return 1;
  }

  neui_clipboard_api_t clipboard_api = {
    NEUI_VERSION,
    cb_set_text,
    cb_get_text,
    cb_has_text,
    cb_read,
    cb_create_item,
    cb_release,
    cb_write,
    cb_item_set_format,
    cb_item_get_format,
    cb_item_has_format,
    cb_item_set_format_callback,
  };

  // ---------------------------------------------------------------------------
  // Asset API (NEUI_API_ASSETS) - session-scoped media handles backed by
  // the win32-host W32AssetManager. Cross-session handles are rejected.

  static neui_asset_t pack_asset_w32(uint32_t session_id, uint32_t slot)
  {
    return { ((session_id & 0xffff) << 16) | (slot & 0xffff) };
  }

  // Win32 native host extra font prong (Part E): a DirectWrite custom
  // collection does NOT expose a face to GDI CreateFontW, so native HFONT
  // widgets (Edit / Button / ...) need a second registration via
  // AddFontMemResourceEx / AddFontResourceExW(FR_PRIVATE). The removable
  // handle (memory) or the private path (file) is kept here keyed by the
  // packed asset id and undone in as_destroy. Process-global, so a session
  // torn down without per-asset destroy leaves the registration until exit -
  // acceptable (the family stays resolvable, matching the backend prong).
  struct W32GdiFont { HANDLE mem = nullptr; std::wstring path; };
  static std::unordered_map<uint32_t, W32GdiFont>& w32_gdi_fonts()
  {
    static std::unordered_map<uint32_t, W32GdiFont> m;
    return m;
  }

  static neui_asset_t NEUI_ABI as_create_bitmap(neui_session_t session,
                                                  uint32_t width_px,
                                                  uint32_t height_px,
                                                  const uint8_t* bgra,
                                                  float scale)
  {
    auto* s = get_session(session);
    if (!s) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_bitmap(width_px, height_px,
                                                       bgra, scale);
    if (slot == 0) return asset_none;
    return pack_asset_w32(s->session_id(), slot);
  }

  // Best-guess @Nx scale for a file load: the screen DPI. Falls back to 1.0.
  static float best_asset_scale_w32()
  {
    float scale = 1.0f;
    HDC screen = GetDC(nullptr);
    if (screen) {
      UINT dpi = GetDeviceCaps(screen, LOGPIXELSX);
      ReleaseDC(nullptr, screen);
      if (dpi > 0) {
        float s_now = static_cast<float>(dpi) / 96.0f;
        if (s_now > scale) scale = s_now;
      }
    }
    return scale;
  }

  static neui_asset_t NEUI_ABI as_create_from_file(neui_session_t session,
                                                     const char* path_utf8)
  {
    auto* s = get_session(session);
    if (!s || !path_utf8) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_from_file(path_utf8,
                                                         best_asset_scale_w32());
    if (slot == 0) return asset_none;
    return pack_asset_w32(s->session_id(), slot);
  }

  static void NEUI_ABI as_destroy(neui_session_t session, neui_asset_t asset)
  {
    auto* s = get_session(session);
    if (!s) return;
    if (asset.id == asset_none.id) return;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return;
    // Undo the GDI font prong (if this asset was a registered font).
    auto& gdi = w32_gdi_fonts();
    auto git = gdi.find(asset.id);
    if (git != gdi.end()) {
      if (git->second.mem) RemoveFontMemResourceEx(git->second.mem);
      else if (!git->second.path.empty())
        RemoveFontResourceExW(git->second.path.c_str(), FR_PRIVATE, nullptr);
      gdi.erase(git);
    }
    s->_asset_manager.release_slot(asset.id & 0xffff,
                                    neui_d2d_backend::get_backend());
  }

  static bool NEUI_ABI as_get_size(neui_session_t session, neui_asset_t asset,
                                     float* out_w, float* out_h)
  {
    auto* s = get_session(session);
    if (!s) return false;
    if (asset.id == asset_none.id) return false;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return false;
    auto* e = s->_asset_manager.get_slot(asset.id & 0xffff);
    if (!e) return false;
    if (e->kind == NEUI_ASSET_KIND_COMPONENT) {
      if (out_w) *out_w = e->comp_w;
      if (out_h) *out_h = e->comp_h;
      return true;
    }
    if (e->scale <= 0.0f) return false;
    if (out_w) *out_w = static_cast<float>(e->width_px)  / e->scale;
    if (out_h) *out_h = static_cast<float>(e->height_px) / e->scale;
    return true;
  }

  static neui_asset_kind_t NEUI_ABI as_get_kind(neui_session_t session,
                                                  neui_asset_t asset)
  {
    auto* s = get_session(session);
    if (!s) return NEUI_ASSET_KIND_NONE;
    if (asset.id == asset_none.id) return NEUI_ASSET_KIND_NONE;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff))
      return NEUI_ASSET_KIND_NONE;
    auto* e = s->_asset_manager.get_slot(asset.id & 0xffff);
    return e ? e->kind : NEUI_ASSET_KIND_NONE;
  }

  static neui_asset_t NEUI_ABI as_create_compound(neui_session_t session)
  {
    auto* s = get_session(session);
    if (!s) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_compound();
    if (slot == 0) return asset_none;
    return pack_asset_w32(s->session_id(), slot);
  }

  static neui_asset_t NEUI_ABI as_create_behavior(neui_session_t session)
  {
    auto* s = get_session(session);
    if (!s) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_behavior();
    if (slot == 0) return asset_none;
    return pack_asset_w32(s->session_id(), slot);
  }

  static neui_asset_t NEUI_ABI as_create_filter(neui_session_t session)
  {
    auto* s = get_session(session);
    if (!s) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_filter();
    if (slot == 0) return asset_none;
    return pack_asset_w32(s->session_id(), slot);
  }

  static void NEUI_ABI as_apply_filter(neui_session_t session,
                                       neui_asset_t surface, neui_asset_t filter)
  {
    auto* s = get_session(session);
    if (!s) return;
    if (surface.id == asset_none.id || filter.id == asset_none.id) return;
    if (((surface.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return;
    if (((filter.id  >> 16) & 0xffff) != (s->session_id() & 0xffff)) return;
    s->_asset_manager.apply_filter(surface.id & 0xffff, filter.id & 0xffff,
                                   neui_d2d_backend::get_backend());
  }

  static neui_asset_t NEUI_ABI as_create_surface(neui_session_t session,
                                                   float width_logical,
                                                   float height_logical,
                                                   float scale)
  {
    auto* s = get_session(session);
    if (!s) return asset_none;
    if (width_logical <= 0.0f || height_logical <= 0.0f) return asset_none;
    if (scale <= 0.0f) scale = 1.0f;
    uint32_t w_px = static_cast<uint32_t>(width_logical  * scale + 0.5f);
    uint32_t h_px = static_cast<uint32_t>(height_logical * scale + 0.5f);
    uint32_t slot = s->_asset_manager.allocate_surface(
      w_px, h_px, scale, neui_d2d_backend::get_backend());
    if (slot == 0) return asset_none;
    return pack_asset_w32(s->session_id(), slot);
  }

  static void NEUI_ABI as_paint_surface(neui_session_t        session,
                                          neui_asset_t          surface,
                                          uint32_t              clear_argb,
                                          neui_surface_paint_fn fn,
                                          void*                 user)
  {
    auto* s = get_session(session);
    if (!s) return;
    if (surface.id == asset_none.id) return;
    if (((surface.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return;
    s->_asset_manager.paint_surface(surface.id & 0xffff, clear_argb, fn, user,
                                     neui_d2d_backend::get_backend(),
                                     /*host_token*/ s,
                                     &w32_painter_draw_asset_thunk);
  }

  static void NEUI_ABI as_surface_blur(neui_session_t session, neui_asset_t surface,
                                        float sigma_x, float sigma_y)
  {
    auto* s = get_session(session);
    if (!s || surface.id == asset_none.id) return;
    if (((surface.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return;
    s->_asset_manager.blur_surface(surface.id & 0xffff, sigma_x, sigma_y,
                                   neui_d2d_backend::get_backend());
  }

  static void NEUI_ABI as_surface_drop_shadow(neui_session_t session, neui_asset_t surface,
                                              float dx, float dy, float sigma,
                                              uint32_t shadow_argb)
  {
    auto* s = get_session(session);
    if (!s || surface.id == asset_none.id) return;
    if (((surface.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return;
    s->_asset_manager.drop_shadow_surface(surface.id & 0xffff, dx, dy, sigma, shadow_argb,
                                          neui_d2d_backend::get_backend());
  }

#define NEUI_W32_SURF_GUARD \
    auto* s = get_session(session); \
    if (!s || surface.id == asset_none.id) return; \
    if (((surface.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return

  static void NEUI_ABI as_surface_inner_shadow(neui_session_t session, neui_asset_t surface,
                                               float dx, float dy, float sigma, uint32_t shadow_argb)
  { NEUI_W32_SURF_GUARD; s->_asset_manager.inner_shadow_surface(surface.id & 0xffff, dx, dy, sigma,
                                                                shadow_argb, neui_d2d_backend::get_backend()); }
  static void NEUI_ABI as_surface_glow(neui_session_t session, neui_asset_t surface,
                                       float sigma, uint32_t glow_argb)
  { NEUI_W32_SURF_GUARD; s->_asset_manager.glow_surface(surface.id & 0xffff, sigma, glow_argb,
                                                        neui_d2d_backend::get_backend()); }
  static void NEUI_ABI as_surface_tint(neui_session_t session, neui_asset_t surface, uint32_t argb)
  { NEUI_W32_SURF_GUARD; s->_asset_manager.tint_surface(surface.id & 0xffff, argb,
                                                        neui_d2d_backend::get_backend()); }
  static void NEUI_ABI as_surface_desaturate(neui_session_t session, neui_asset_t surface, float amount)
  { NEUI_W32_SURF_GUARD; s->_asset_manager.desaturate_surface(surface.id & 0xffff, amount,
                                                              neui_d2d_backend::get_backend()); }
  static void NEUI_ABI as_surface_elevation(neui_session_t session, neui_asset_t surface, float level)
  { NEUI_W32_SURF_GUARD; s->_asset_manager.elevation_surface(surface.id & 0xffff, level,
                                                             neui_d2d_backend::get_backend()); }
  static void NEUI_ABI as_surface_bevel(neui_session_t session, neui_asset_t surface,
                                        float dx, float dy, float sigma,
                                        uint32_t light_argb, uint32_t dark_argb)
  { NEUI_W32_SURF_GUARD; s->_asset_manager.bevel_surface(surface.id & 0xffff, dx, dy, sigma,
                                                         light_argb, dark_argb, neui_d2d_backend::get_backend()); }
#undef NEUI_W32_SURF_GUARD

  static neui_asset_t NEUI_ABI as_create_font(neui_session_t session,
                                               const uint8_t* data, uint32_t len)
  {
    auto* s = get_session(session);
    if (!s || !data || len == 0) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_font(
      data, len, neui_d2d_backend::get_backend());
    if (slot == 0) return asset_none;
    neui_asset_t a = pack_asset_w32(s->session_id(), slot);
    // GDI prong for native HFONT widgets (AddFontMemResourceEx copies data).
    DWORD count = 0;
    HANDLE h = AddFontMemResourceEx(const_cast<uint8_t*>(data), len,
                                     nullptr, &count);
    if (h) w32_gdi_fonts()[a.id] = W32GdiFont{ h, {} };
    return a;
  }

  static neui_asset_t NEUI_ABI as_create_font_from_file(neui_session_t session,
                                                        const char* path_utf8)
  {
    auto* s = get_session(session);
    if (!s || !path_utf8) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_font_from_file(
      path_utf8, neui_d2d_backend::get_backend());
    if (slot == 0) return asset_none;
    neui_asset_t a = pack_asset_w32(s->session_id(), slot);
    // GDI prong: register the file privately for native HFONT widgets.
    std::wstring wpath;
    int need = MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, nullptr, 0);
    if (need > 1) {
      wpath.resize(need - 1);
      MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, wpath.data(), need);
      if (AddFontResourceExW(wpath.c_str(), FR_PRIVATE, nullptr) > 0)
        w32_gdi_fonts()[a.id] = W32GdiFont{ nullptr, wpath };
    }
    return a;
  }

  static uint32_t NEUI_ABI as_get_font_family(neui_session_t session,
                                              neui_asset_t font,
                                              char* out_buf, uint32_t cap)
  {
    auto* s = get_session(session);
    if (!s || font.id == asset_none.id) return 0;
    if (((font.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return 0;
    return s->_asset_manager.get_font_family(font.id & 0xffff, out_buf, cap);
  }

  // Asset / compound / behavior tables (compound_api / behavior_api defined
  // later in this TU; asset_api just below). Forward-declared so the component
  // thunks can hand all three to build_component.
  extern neui_asset_api_t    asset_api;
  extern neui_compound_api_t compound_api;
  extern neui_behavior_api_t behavior_api;

  static void release_built_component_w32(neui_session_t session,
                                          neui_detail::BuiltComponent& built)
  {
    if (built.compound.id != asset_none.id) as_destroy(session, built.compound);
    if (built.behavior.id != asset_none.id) as_destroy(session, built.behavior);
    for (auto a : built.owned_assets) as_destroy(session, a);
  }

  static neui_asset_t NEUI_ABI as_create_component_from_string(
      neui_session_t session, const char* json, uint32_t len,
      const neui_component_env_t* env)
  {
    auto* s = get_session(session);
    if (!s || !json) return asset_none;
    neui_detail::ComponentApis apis;
    apis.asset    = &asset_api;
    apis.compound = &compound_api;
    apis.behavior = &behavior_api;
    neui_detail::BuiltComponent built =
        neui_detail::build_component(session, json, len, env, apis);
    if (!built.ok) { release_built_component_w32(session, built); return asset_none; }
    uint32_t slot = s->_asset_manager.allocate_component(built);
    if (slot == 0) { release_built_component_w32(session, built); return asset_none; }
    return pack_asset_w32(s->session_id(), slot);
  }

  static neui_asset_t NEUI_ABI as_create_component_from_file(
      neui_session_t session, const char* path_utf8,
      const neui_component_env_t* env)
  {
    auto* s = get_session(session);
    if (!s || !path_utf8) return asset_none;
    std::ifstream in(path_utf8, std::ios::binary);
    if (!in) return asset_none;
    std::string data((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    neui_component_env_t local{};
    const neui_component_env_t* use_env = env;
    static thread_local std::string base_keep;
    if (!env || !env->base_dir) {
      std::string p = path_utf8;
      size_t cut = p.find_last_of("/\\");
      base_keep = (cut == std::string::npos) ? std::string() : p.substr(0, cut);
      if (env) local = *env;
      local.base_dir = base_keep.c_str();
      use_env = &local;
    }
    return as_create_component_from_string(session, data.c_str(),
                                           static_cast<uint32_t>(data.size()),
                                           use_env);
  }

  static uint32_t NEUI_ABI as_component_param_count(neui_session_t session,
                                                    neui_asset_t component)
  {
    auto* s = get_session(session);
    if (!s || component.id == asset_none.id) return 0;
    if (((component.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return 0;
    auto* e = s->_asset_manager.get_slot(component.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_COMPONENT) return 0;
    return static_cast<uint32_t>(e->comp_params.size());
  }

  static bool NEUI_ABI as_component_param_at(neui_session_t session,
                                             neui_asset_t component,
                                             uint32_t index,
                                             neui_component_param_t* out)
  {
    auto* s = get_session(session);
    if (!s || !out || component.id == asset_none.id) return false;
    if (((component.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return false;
    auto* e = s->_asset_manager.get_slot(component.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_COMPONENT) return false;
    if (index >= e->comp_params.size()) return false;
    const auto& p = e->comp_params[index];
    out->key = p.key.c_str(); out->label = p.label.c_str();
    out->min = p.min; out->max = p.max; out->def = p.def;
    return true;
  }

  static uint32_t NEUI_ABI as_serialize_component(neui_session_t session,
                                                  neui_asset_t component,
                                                  char* out_buf, uint32_t cap,
                                                  int indent)
  {
    auto* s = get_session(session);
    if (!s || component.id == asset_none.id) return 0;
    if (((component.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return 0;
    auto* e = s->_asset_manager.get_slot(component.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_COMPONENT) return 0;
    neui_detail::ComponentSerializeInput in;
    in.name = &e->comp_name; in.width = e->comp_w; in.height = e->comp_h;
    in.params = &e->comp_params;
    in.asset_names = &e->comp_asset_names;
    in.asset_handle_names = &e->comp_asset_handle_names;
    in.asset_frame_layouts = &e->comp_asset_frame_layouts;
    auto* cce = s->_asset_manager.get_slot(e->comp_compound.id & 0xffff);
    auto* bbe = s->_asset_manager.get_slot(e->comp_behavior.id & 0xffff);
    in.compound = (cce && cce->compound) ? cce->compound.get() : nullptr;
    in.behavior = (bbe && bbe->behavior) ? bbe->behavior.get() : nullptr;
    std::string json = neui_detail::serialize_component(in, indent);
    uint32_t full = static_cast<uint32_t>(json.size());
    if (out_buf && cap > 0) {
      uint32_t n = (full > cap - 1) ? cap - 1 : full;
      if (n) std::memcpy(out_buf, json.data(), n);
      out_buf[n] = '\0';
    }
    return full;
  }

  static bool NEUI_ABI as_set_frame_layout(neui_session_t session,
                                           neui_asset_t asset,
                                           uint32_t cols, uint32_t rows,
                                           uint32_t gutter_px)
  {
    auto* s = get_session(session);
    if (!s || asset.id == asset_none.id) return false;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return false;
    return s->_asset_manager.set_frame_layout(asset.id & 0xffff,
                                              cols, rows, gutter_px);
  }

  static neui_asset_t NEUI_ABI as_create_filmstrip_from_file(
      neui_session_t session, const char* path_utf8,
      uint32_t frame_count, neui_filmstrip_orientation_t orientation)
  {
    auto* s = get_session(session);
    if (!s || !path_utf8) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_filmstrip_from_file(
        path_utf8, best_asset_scale_w32(), frame_count,
        orientation == NEUI_FILMSTRIP_HORIZONTAL,
        neui_d2d_backend::get_backend());
    if (slot == 0) return asset_none;
    return pack_asset_w32(s->session_id(), slot);
  }

  static uint32_t NEUI_ABI as_get_frame_count(neui_session_t session,
                                              neui_asset_t asset)
  {
    auto* s = get_session(session);
    if (!s || asset.id == asset_none.id) return 0;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return 0;
    return s->_asset_manager.frame_count(asset.id & 0xffff);
  }

  neui_asset_api_t asset_api = {
    NEUI_VERSION,
    as_create_bitmap,
    as_create_from_file,
    as_destroy,
    as_get_size,
    as_get_kind,
    as_create_compound,
    as_create_behavior,
    as_create_surface,
    as_paint_surface,
    as_create_font,
    as_create_font_from_file,
    as_get_font_family,
    as_create_component_from_string,
    as_create_component_from_file,
    as_component_param_count,
    as_component_param_at,
    as_serialize_component,
    as_set_frame_layout,
    as_create_filmstrip_from_file,
    as_get_frame_count,
    as_surface_blur,
    as_surface_drop_shadow,
    as_create_filter,
    as_apply_filter,
    as_surface_inner_shadow,
    as_surface_glow,
    as_surface_tint,
    as_surface_desaturate,
    as_surface_elevation,
    as_surface_bevel,
  };

  // ---------------------------------------------------------------------------
  // Compound API (NEUI_API_COMPOUND) - same shape as the xpl host's
  // compound_api. Mutators dispatch to the shared mutator helpers in
  // hosts/shared/compound.h; the host-side concern here is handle
  // validation, looking up the W32AssetEntry's compound storage, and
  // invalidating attached CUSTOMDRAW widgets after each mutation.

  static neui_detail::CompoundLayer*
  resolve_layer_w32(neui_session_t session, neui_asset_t asset,
                     neui_compound_layer_t layer, Session*& out_session)
  {
    out_session = nullptr;
    auto* s = get_session(session);
    if (!s) return nullptr;
    if (asset.id == asset_none.id) return nullptr;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return nullptr;
    uint32_t asset_slot = asset.id & 0xffff;
    if (neui_detail::compound_layer_asset_slot(layer) != asset_slot) return nullptr;
    auto* e = s->_asset_manager.get_slot(asset_slot);
    if (!e || e->kind != NEUI_ASSET_KIND_COMPOUND || !e->compound) return nullptr;
    out_session = s;
    return neui_detail::compound_get_layer(*e->compound,
                                            neui_detail::compound_layer_slot(layer));
  }

  static neui_detail::CompoundAsset*
  resolve_compound_w32(neui_session_t session, neui_asset_t asset, Session*& out_session)
  {
    out_session = nullptr;
    auto* s = get_session(session);
    if (!s) return nullptr;
    if (asset.id == asset_none.id) return nullptr;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return nullptr;
    auto* e = s->_asset_manager.get_slot(asset.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_COMPOUND || !e->compound) return nullptr;
    out_session = s;
    return e->compound.get();
  }

  static neui_compound_layer_t NEUI_ABI co_add_layer(neui_session_t session,
                                                       neui_asset_t asset,
                                                       neui_compound_layer_kind_t kind,
                                                       int z)
  {
    Session* s = nullptr;
    auto* ca = resolve_compound_w32(session, asset, s);
    if (!ca) return compound_layer_none;
    uint32_t asset_slot = asset.id & 0xffff;
    uint32_t slot = neui_detail::compound_add_layer(*ca, kind, z);
    s->invalidate_widgets_with_compound(asset.id);
    return neui_detail::pack_compound_layer(asset_slot, slot);
  }

  static neui_compound_layer_t NEUI_ABI co_add_child_layer(neui_session_t session,
                                                           neui_asset_t asset,
                                                           neui_compound_layer_t parent,
                                                           neui_compound_layer_kind_t kind,
                                                           int z)
  {
    Session* s = nullptr;
    auto* ca = resolve_compound_w32(session, asset, s);
    if (!ca) return compound_layer_none;
    uint32_t asset_slot = asset.id & 0xffff;
    if (neui_detail::compound_layer_asset_slot(parent) != asset_slot)
      return compound_layer_none;
    uint32_t slot = neui_detail::compound_add_child_layer(
      *ca, neui_detail::compound_layer_slot(parent), kind, z);
    if (slot == UINT32_MAX) return compound_layer_none;
    s->invalidate_widgets_with_compound(asset.id);
    return neui_detail::pack_compound_layer(asset_slot, slot);
  }

  static void NEUI_ABI co_remove_layer(neui_session_t session, neui_asset_t asset,
                                         neui_compound_layer_t layer)
  {
    Session* s = nullptr;
    auto* ca = resolve_compound_w32(session, asset, s);
    if (!ca) return;
    if (neui_detail::compound_layer_asset_slot(layer) != (asset.id & 0xffff)) return;
    neui_detail::compound_remove_layer(*ca, neui_detail::compound_layer_slot(layer));
    s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_clear(neui_session_t session, neui_asset_t asset)
  {
    Session* s = nullptr;
    auto* ca = resolve_compound_w32(session, asset, s);
    if (!ca) return;
    neui_detail::compound_clear(*ca);
    s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_set_z(neui_session_t session, neui_asset_t asset,
                                  neui_compound_layer_t layer, int z)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_w32(session, asset, layer, s);
    if (!L) return;
    L->z = z;
    s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_set_anchor(neui_session_t session, neui_asset_t asset,
                                       neui_compound_layer_t layer,
                                       neui_anchor_t parent_anchor,
                                       neui_anchor_t self_anchor)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_w32(session, asset, layer, s);
    if (!L) return;
    L->parent_anchor = parent_anchor;
    L->self_anchor   = self_anchor;
    s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_set_int(neui_session_t session, neui_asset_t asset,
                                    neui_compound_layer_t layer,
                                    const char* prop, int value)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_w32(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_set_int(*L, prop, value);
    s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_set_float(neui_session_t session, neui_asset_t asset,
                                      neui_compound_layer_t layer,
                                      const char* prop, float value)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_w32(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_set_float(*L, prop, value);
    s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_set_string(neui_session_t session, neui_asset_t asset,
                                       neui_compound_layer_t layer,
                                       const char* prop, const char* value)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_w32(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_set_string(*L, prop, value);
    s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_set_asset(neui_session_t session, neui_asset_t asset,
                                      neui_compound_layer_t layer,
                                      const char* prop, neui_asset_t value)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_w32(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_set_asset(*L, prop, value);
    s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_bind(neui_session_t session, neui_asset_t asset,
                                 neui_compound_layer_t layer,
                                 const char* prop, const char* attr_key,
                                 float scale, float offset)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_w32(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_bind(*L, prop, attr_key, scale, offset);
    s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_bind_asset(neui_session_t session, neui_asset_t asset,
                                       neui_compound_layer_t layer,
                                       const char* prop, const char* attr_key)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_w32(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_bind_asset(*L, prop, attr_key);
    s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_unbind(neui_session_t session, neui_asset_t asset,
                                   neui_compound_layer_t layer, const char* prop)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_w32(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_unbind(*L, prop);
    s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_set_path(neui_session_t session, neui_asset_t asset,
                                     neui_compound_layer_t layer,
                                     const neui_path_cmd_t* cmds,
                                     uint32_t count)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_w32(session, asset, layer, s);
    if (!L) return;
    neui_detail::apply_set_path(*L, cmds, count);
    s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_set_gradient(neui_session_t session, neui_asset_t asset,
                                       neui_compound_layer_t layer,
                                       const neui_gradient_t* grad)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_w32(session, asset, layer, s);
    if (!L) return;
    neui_detail::apply_set_gradient(*L, grad);
    s->invalidate_widgets_with_compound(asset.id);
  }

  neui_compound_api_t compound_api = {
    NEUI_VERSION,
    co_add_layer,
    co_remove_layer,
    co_clear,
    co_set_z,
    co_set_anchor,
    co_set_int,
    co_set_float,
    co_set_string,
    co_set_asset,
    co_bind,
    co_bind_asset,
    co_unbind,
    co_set_path,
    co_set_gradient,
    co_add_child_layer,
  };

  // ---------------------------------------------------------------------------
  // Behavior API (NEUI_API_BEHAVIOR) - same shape as compound_api. Mutations
  // don't change paint output, so no invalidate walk is needed; the per-write
  // invalidate happens in the dispatch callbacks at run time.

  static neui_detail::BehaviorAsset*
  resolve_behavior_w32(neui_session_t session, neui_asset_t asset, Session*& out_session)
  {
    out_session = nullptr;
    auto* s = get_session(session);
    if (!s) return nullptr;
    if (asset.id == asset_none.id) return nullptr;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return nullptr;
    auto* e = s->_asset_manager.get_slot(asset.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_BEHAVIOR || !e->behavior) return nullptr;
    out_session = s;
    return e->behavior.get();
  }

  static neui_detail::BehaviorHandler*
  resolve_behavior_handler_w32(neui_session_t session, neui_asset_t asset,
                                 neui_behavior_handler_t handler, Session*& out_session)
  {
    out_session = nullptr;
    auto* s = get_session(session);
    if (!s) return nullptr;
    if (asset.id == asset_none.id) return nullptr;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return nullptr;
    uint32_t asset_slot = asset.id & 0xffff;
    if (neui_detail::behavior_handler_asset_slot(handler) != asset_slot) return nullptr;
    auto* e = s->_asset_manager.get_slot(asset_slot);
    if (!e || e->kind != NEUI_ASSET_KIND_BEHAVIOR || !e->behavior) return nullptr;
    out_session = s;
    return neui_detail::behavior_get_handler(*e->behavior,
                                              neui_detail::behavior_handler_slot(handler));
  }

  static neui_behavior_handler_t NEUI_ABI be_add_handler(neui_session_t session,
                                                          neui_asset_t asset,
                                                          neui_behavior_kind_t kind)
  {
    Session* s = nullptr;
    auto* ba = resolve_behavior_w32(session, asset, s);
    if (!ba) return behavior_handler_none;
    uint32_t asset_slot = asset.id & 0xffff;
    uint32_t slot = neui_detail::behavior_add_handler(*ba, kind);
    return neui_detail::pack_behavior_handler(asset_slot, slot);
  }

  static void NEUI_ABI be_remove_handler(neui_session_t session, neui_asset_t asset,
                                          neui_behavior_handler_t handler)
  {
    Session* s = nullptr;
    auto* ba = resolve_behavior_w32(session, asset, s);
    if (!ba) return;
    if (neui_detail::behavior_handler_asset_slot(handler) != (asset.id & 0xffff)) return;
    neui_detail::behavior_remove_handler(*ba, neui_detail::behavior_handler_slot(handler));
  }

  static void NEUI_ABI be_clear(neui_session_t session, neui_asset_t asset)
  {
    Session* s = nullptr;
    auto* ba = resolve_behavior_w32(session, asset, s);
    if (!ba) return;
    neui_detail::behavior_clear(*ba);
  }

  static void NEUI_ABI be_set_int(neui_session_t session, neui_asset_t asset,
                                    neui_behavior_handler_t handler,
                                    const char* prop, int value)
  {
    Session* s = nullptr;
    auto* H = resolve_behavior_handler_w32(session, asset, handler, s);
    if (!H || !prop) return;
    neui_detail::apply_behavior_set_int(*H, prop, value);
  }

  static void NEUI_ABI be_set_float(neui_session_t session, neui_asset_t asset,
                                      neui_behavior_handler_t handler,
                                      const char* prop, float value)
  {
    Session* s = nullptr;
    auto* H = resolve_behavior_handler_w32(session, asset, handler, s);
    if (!H || !prop) return;
    neui_detail::apply_behavior_set_float(*H, prop, value);
  }

  static void NEUI_ABI be_set_string(neui_session_t session, neui_asset_t asset,
                                       neui_behavior_handler_t handler,
                                       const char* prop, const char* value)
  {
    Session* s = nullptr;
    auto* H = resolve_behavior_handler_w32(session, asset, handler, s);
    if (!H || !prop) return;
    neui_detail::apply_behavior_set_string(*H, prop, value);
  }

  neui_behavior_api_t behavior_api = {
    NEUI_VERSION,
    be_add_handler,
    be_remove_handler,
    be_clear,
    be_set_int,
    be_set_float,
    be_set_string,
  };

  // -------------------------------------------------------------------------
  // Filter API (NEUI_API_FILTER) - shared FilterAsset graph (filter_graph.h),
  // applied to a SURFACE via assets->apply_filter.

  static neui_detail::FilterAsset*
  resolve_filter_w32(neui_session_t session, neui_asset_t asset, Session*& out_session)
  {
    out_session = nullptr;
    auto* s = get_session(session);
    if (!s) return nullptr;
    if (asset.id == asset_none.id) return nullptr;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return nullptr;
    auto* e = s->_asset_manager.get_slot(asset.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_FILTER || !e->filter) return nullptr;
    out_session = s;
    return e->filter.get();
  }

  static neui_detail::FilterPrimitive*
  resolve_filter_prim_w32(neui_session_t session, neui_asset_t asset,
                          neui_filter_prim_t prim, Session*& out_session)
  {
    auto* fa = resolve_filter_w32(session, asset, out_session);
    if (!fa) return nullptr;
    if (neui_detail::filter_prim_asset_slot(prim) != (asset.id & 0xffff)) return nullptr;
    return neui_detail::filter_get_prim(*fa, neui_detail::filter_prim_slot(prim));
  }

  static neui_filter_prim_t NEUI_ABI fi_add_primitive(neui_session_t session,
                                                      neui_asset_t asset,
                                                      neui_filter_prim_kind_t kind)
  {
    Session* s = nullptr;
    auto* fa = resolve_filter_w32(session, asset, s);
    if (!fa) return filter_prim_none;
    uint32_t slot = neui_detail::filter_add_primitive(*fa, kind);
    return neui_detail::pack_filter_prim(asset.id & 0xffff, slot);
  }
  static void NEUI_ABI fi_remove_primitive(neui_session_t session, neui_asset_t asset,
                                           neui_filter_prim_t prim)
  {
    Session* s = nullptr;
    auto* fa = resolve_filter_w32(session, asset, s);
    if (!fa) return;
    if (neui_detail::filter_prim_asset_slot(prim) != (asset.id & 0xffff)) return;
    neui_detail::filter_remove_primitive(*fa, neui_detail::filter_prim_slot(prim));
  }
  static void NEUI_ABI fi_clear(neui_session_t session, neui_asset_t asset)
  {
    Session* s = nullptr;
    auto* fa = resolve_filter_w32(session, asset, s);
    if (fa) neui_detail::filter_clear(*fa);
  }
  static void NEUI_ABI fi_set_input(neui_session_t session, neui_asset_t asset,
                                    neui_filter_prim_t prim, int slot, const char* source)
  {
    Session* s = nullptr;
    auto* P = resolve_filter_prim_w32(session, asset, prim, s);
    if (P) neui_detail::apply_filter_set_input(*P, slot, source);
  }
  static void NEUI_ABI fi_set_result(neui_session_t session, neui_asset_t asset,
                                     neui_filter_prim_t prim, const char* name)
  {
    Session* s = nullptr;
    auto* P = resolve_filter_prim_w32(session, asset, prim, s);
    if (P) neui_detail::apply_filter_set_result(*P, name);
  }
  static void NEUI_ABI fi_set_region(neui_session_t session, neui_asset_t asset,
                                     neui_filter_prim_t prim,
                                     float x, float y, float w, float h)
  {
    Session* s = nullptr;
    auto* P = resolve_filter_prim_w32(session, asset, prim, s);
    if (P) neui_detail::apply_filter_set_region(*P, x, y, w, h);
  }
  static void NEUI_ABI fi_set_int(neui_session_t session, neui_asset_t asset,
                                  neui_filter_prim_t prim, const char* prop, int value)
  {
    Session* s = nullptr;
    auto* P = resolve_filter_prim_w32(session, asset, prim, s);
    if (P && prop) neui_detail::apply_filter_set_int(*P, prop, value);
  }
  static void NEUI_ABI fi_set_float(neui_session_t session, neui_asset_t asset,
                                    neui_filter_prim_t prim, const char* prop, float value)
  {
    Session* s = nullptr;
    auto* P = resolve_filter_prim_w32(session, asset, prim, s);
    if (P && prop) neui_detail::apply_filter_set_float(*P, prop, value);
  }
  static void NEUI_ABI fi_set_string(neui_session_t session, neui_asset_t asset,
                                     neui_filter_prim_t prim, const char* prop, const char* value)
  {
    Session* s = nullptr;
    auto* P = resolve_filter_prim_w32(session, asset, prim, s);
    if (P && prop) neui_detail::apply_filter_set_string(*P, prop, value);
  }
  static void NEUI_ABI fi_set_floats(neui_session_t session, neui_asset_t asset,
                                     neui_filter_prim_t prim, const char* prop,
                                     const float* values, uint32_t count)
  {
    Session* s = nullptr;
    auto* P = resolve_filter_prim_w32(session, asset, prim, s);
    if (P && prop) neui_detail::apply_filter_set_floats(*P, prop, values, count);
  }
  static void NEUI_ABI fi_merge_add_input(neui_session_t session, neui_asset_t asset,
                                          neui_filter_prim_t prim, const char* source)
  {
    Session* s = nullptr;
    auto* P = resolve_filter_prim_w32(session, asset, prim, s);
    if (P) neui_detail::apply_filter_merge_add_input(*P, source);
  }

  neui_filter_api_t filter_api = {
    NEUI_VERSION,
    fi_add_primitive,
    fi_remove_primitive,
    fi_clear,
    fi_set_input,
    fi_set_result,
    fi_set_region,
    fi_set_int,
    fi_set_float,
    fi_set_string,
    fi_set_floats,
    fi_merge_add_input,
  };

  // -------------------------------------------------------------------------
  // GRID (NEUI_W_GRID) - hosted on the shared "neui.painted" class.
  // Visual state + dispatch model live in hosts/shared/grid_model.h and
  // hosts/shared/widget_paint_grid.h; this file only carries the win32
  // glue (paint_fn, painted_msg_fn, the C API table).

  // Cursor seam. The shape set (enum neui_cursor_kind) and the kind->HCURSOR
  // table are shared with the xpl host - see ../shared/win32/cursor_win32.h.
  // This host previously kept a two-entry copy of the list; it did not survive
  // the set being widened, which is exactly why the table moved to shared.
  static void platform_set_cursor_w32(int kind)
  {
    neui_detail::win32_apply_cursor(kind);
  }

  // Lazy-allocate the model so non-GRID widgets pay only a pointer.
  static neui_detail::GridModel& ensure_grid_model_w32(WidgetData& wd)
  {
    if (!wd.grid_model)
      wd.grid_model = std::make_unique<neui_detail::GridModel>();
    return *wd.grid_model;
  }

  // 60 Hz spring-back timer for SMOOTH-mode grid scroll. Per-HWND so different
  // grids tick independently; chosen well outside any other timer ID space we
  // might use later (none today).
  static constexpr UINT_PTR GRID_BOUNCE_TIMER_ID = 0x6E677562;  // 'ngub'

  // Convert a physical-pixel HWND-local coord to a logical-pixel one.
  static int phys_to_log_w32(int physical, UINT dpi)
  {
    if (dpi == 0) dpi = 96;
    return MulDiv(physical, 96, static_cast<int>(dpi));
  }

  // -------- Event dispatch helpers ---------------------------------------

  static bool grid_fire_row_selected_w32(WidgetData& wd, int row)
  {
    if (!wd.session) return false;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_GRID_ROW_SELECTED;
    ev.data.grid_row.widget.id = wd.widget_id;
    ev.data.grid_row.row       = row;
    return wd.session->dispatch_event(&ev);
  }

  static bool grid_fire_cell_selected_w32(WidgetData& wd, int row, int col)
  {
    if (!wd.session) return false;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_GRID_CELL_SELECTED;
    ev.data.grid_cell.widget.id = wd.widget_id;
    ev.data.grid_cell.row       = row;
    ev.data.grid_cell.col       = col;
    return wd.session->dispatch_event(&ev);
  }

  static bool grid_fire_cell_clicked_w32(WidgetData& wd, int row, int col)
  {
    if (!wd.session) return false;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_GRID_CELL_CLICKED;
    ev.data.grid_cell.widget.id = wd.widget_id;
    ev.data.grid_cell.row       = row;
    ev.data.grid_cell.col       = col;
    return wd.session->dispatch_event(&ev);
  }

  static void grid_fire_row_activated_w32(WidgetData& wd, int row)
  {
    if (!wd.session) return;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_GRID_ROW_ACTIVATED;
    ev.data.grid_row.widget.id = wd.widget_id;
    ev.data.grid_row.row       = row;
    wd.session->dispatch_event(&ev);
  }

  static void grid_fire_column_resized_w32(WidgetData& wd, int col, int old_w, int new_w)
  {
    if (!wd.session) return;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_GRID_COLUMN_RESIZED;
    ev.data.grid_column_resize.widget.id = wd.widget_id;
    ev.data.grid_column_resize.col       = col;
    ev.data.grid_column_resize.old_width = old_w;
    ev.data.grid_column_resize.new_width = new_w;
    wd.session->dispatch_event(&ev);
  }

  static void grid_fire_sort_changed_w32(WidgetData& wd, int col,
                                           neui_grid_sort_dir_t dir)
  {
    if (!wd.session) return;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_GRID_SORT_CHANGED;
    ev.data.grid_sort.widget.id = wd.widget_id;
    ev.data.grid_sort.col       = col;
    ev.data.grid_sort.dir       = (int)dir;
    wd.session->dispatch_event(&ev);
  }

  // Dispatch ladder run after a body cell click: always update selection,
  // then ROW_SELECTED -> (cell_focus ? CELL_SELECTED : skip) -> CELL_CLICKED.
  static void grid_click_ladder_w32(WidgetData& wd, int row, int col)
  {
    auto& m = ensure_grid_model_w32(wd);
    auto cfg = neui_detail::grid_read_config(wd.attrs.get());
    m.selected_row = row;
    if (cfg.cell_focus) m.selected_col = col;
    if (grid_fire_row_selected_w32(wd, row)) return;
    if (cfg.cell_focus) {
      if (grid_fire_cell_selected_w32(wd, row, col)) return;
    }
    grid_fire_cell_clicked_w32(wd, row, col);
  }

  // ---- Cell-edit dispatch helpers (win32) --------------------------------

  // Forward declarations - both helpers and the painted_msg below live in
  // the same TU but their definitions interleave.
  static void grid_repaint_w32(WidgetData& wd);

  static void grid_fire_cell_edit_event_w32(WidgetData& wd, neui_event_type_t t,
                                              int row, int col)
  {
    if (!wd.session) return;
    neui_event_t ev{};
    ev.type = t;
    ev.data.grid_cell.widget.id = wd.widget_id;
    ev.data.grid_cell.row       = row;
    ev.data.grid_cell.col       = col;
    wd.session->dispatch_event(&ev);
  }

  static bool grid_try_begin_edit_w32(WidgetData& wd, int row, int col)
  {
    auto& m = ensure_grid_model_w32(wd);
    if (m.edit.active) return false;
    auto cfg = neui_detail::grid_read_config(wd.attrs.get());
    if (!neui_detail::grid_cell_edit_allowed(m, row, col, cfg.cell_focus))
      return false;
    neui_detail::grid_begin_edit(m, row, col);
    grid_repaint_w32(wd);
    grid_fire_cell_edit_event_w32(wd, NEUI_EVENT_GRID_CELL_EDIT_BEGIN, row, col);
    return true;
  }

  static bool grid_commit_edit_w32(WidgetData& wd)
  {
    auto& m = ensure_grid_model_w32(wd);
    if (!m.edit.active) return false;
    int row = m.edit.row;
    int col = m.edit.col;
    auto* client = wd.session ? wd.session->_grid_client : nullptr;
    const std::string proposed = m.edit.te.text;
    if (client && client->validate_cell) {
      neui_widget_t w{}; w.id = wd.widget_id;
      if (!client->validate_cell(wd.session->get_token(), w, row, col,
                                   proposed.c_str())) {
        return false;
      }
    }
    (void)neui_detail::grid_end_edit(m);
    auto& r = m.rows[(size_t)row];
    if ((int)r.cells.size() <= col) r.cells.resize((size_t)col + 1);
    r.cells[(size_t)col] = proposed;
    m.sort_dirty = true;
    grid_repaint_w32(wd);
    grid_fire_cell_edit_event_w32(wd, NEUI_EVENT_GRID_CELL_CHANGED, row, col);
    return true;
  }

  static void grid_cancel_edit_w32(WidgetData& wd)
  {
    auto& m = ensure_grid_model_w32(wd);
    if (!m.edit.active) return;
    int row = m.edit.row;
    int col = m.edit.col;
    (void)neui_detail::grid_end_edit(m);
    grid_repaint_w32(wd);
    grid_fire_cell_edit_event_w32(wd, NEUI_EVENT_GRID_CELL_EDIT_CANCEL, row, col);
  }


  // -------- paint_fn ------------------------------------------------------

  static void paint_grid_w32(neui_render_backend_t* backend,
                              neui_render_ctx_t      ctx,
                              float w, float h,
                              WidgetData&            wd,
                              bool                   focused)
  {
    auto& m = ensure_grid_model_w32(wd);
    neui_detail::paint_grid(backend, ctx, 0.0f, 0.0f, w, h,
                              m, wd.attrs.get(), focused);
  }

  // -------- painted_msg_fn -----------------------------------------------

  // Compute the GridViewport for the current widget, taking the live HWND
  // client-rect size in logical pixels (so it tracks resizes).
  static neui_detail::GridViewport grid_viewport_w32(WidgetData& wd,
                                                       UINT* out_dpi = nullptr)
  {
    UINT dpi = wd.session ? wd.session->get_dpi_for_widget(wd.index) : 96;
    if (dpi == 0) dpi = 96;
    if (out_dpi) *out_dpi = dpi;
    auto& m = ensure_grid_model_w32(wd);
    auto cfg = neui_detail::grid_read_config(wd.attrs.get());
    RECT rc{};
    if (wd.hwnd) GetClientRect(wd.hwnd, &rc);
    int lw = phys_to_log_w32(rc.right,  dpi);
    int lh = phys_to_log_w32(rc.bottom, dpi);
    return neui_detail::grid_compute_viewport(m, lw, lh, cfg.row_h, cfg.header_h);
  }

  static void grid_repaint_w32(WidgetData& wd)
  {
    if (wd.hwnd) InvalidateRect(wd.hwnd, nullptr, FALSE);
  }

  static void painted_msg_grid_w32(WidgetData& wd, UINT msg,
                                     WPARAM wParam, LPARAM lParam)
  {
    using namespace neui_detail;
    if (!wd.session) return;
    auto& m = ensure_grid_model_w32(wd);
    auto cfg = grid_read_config(wd.attrs.get());

    // Tear-down: cancel any running bounce timer so the HWND-keyed slot stays
    // clean across slot reuse.
    if (msg == WM_DESTROY) {
      if (wd.hwnd) KillTimer(wd.hwnd, GRID_BOUNCE_TIMER_ID);
      return;
    }

    // Focus loss (Tab-traversal, mouse click elsewhere, etc.) commits an
    // open in-place editor as if the user had pressed Enter. If the client
    // validate_cell rejects the value, we fall back to cancelling rather
    // than fighting Win32's focus change.
    if (msg == WM_KILLFOCUS && m.edit.active) {
      if (!grid_commit_edit_w32(wd))
        grid_cancel_edit_w32(wd);
      return;
    }

    UINT dpi = wd.session->get_dpi_for_widget(wd.index);
    if (dpi == 0) dpi = 96;

    RECT rc{}; if (wd.hwnd) GetClientRect(wd.hwnd, &rc);
    int widget_w = phys_to_log_w32(rc.right,  dpi);
    int widget_h = phys_to_log_w32(rc.bottom, dpi);
    GridViewport vp = grid_compute_viewport(m, widget_w, widget_h,
                                              cfg.row_h, cfg.header_h);

    // Spring-back tick (SMOOTH mode). KillTimer is idempotent so an out-of-
    // order WM_TIMER after the bounce ended is harmless.
    if (msg == WM_TIMER && wParam == GRID_BOUNCE_TIMER_ID) {
      bool more = grid_scroll_bounce_step(m, vp, cfg.row_h);
      grid_repaint_w32(wd);
      if (!more && wd.hwnd) KillTimer(wd.hwnd, GRID_BOUNCE_TIMER_ID);
      return;
    }

    // Wheel - branch on the effective scroll mode. STEPPED (Win32 default) is
    // the historical row-quantized scroll; SMOOTH opts into the shared pixel-
    // precise kinetics with rubber-band overshoot + 60 Hz spring-back.
    if (msg == WM_MOUSEWHEEL) {
      short delta_raw = (short)HIWORD(wParam);
      if (!grid_smooth_enabled(cfg, /*platform_default_smooth=*/false)) {
        int delta_lines = delta_raw / WHEEL_DELTA;
        if (delta_lines == 0) delta_lines = (delta_raw > 0) ? 1 : -1;
        if (wd.hwnd) KillTimer(wd.hwnd, GRID_BOUNCE_TIMER_ID);
        grid_scroll_step_rows(m, vp, cfg.row_h, -delta_lines);
        grid_repaint_w32(wd);
        return;
      }
      // SMOOTH: feed the kinetics as a synthetic single-shot precise wheel.
      // Win32 WM_MOUSEWHEEL has no gesture phase or momentum, so each notch
      // looks like a stand-alone gesture - the rubber-band math reads raw_px
      // overshoot directly from the accumulator and the WM_TIMER springs it
      // back. SPI_GETWHEELSCROLLLINES is read once per event to honour the
      // user's wheel-speed setting.
      UINT lines_per_notch = 3;
      SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &lines_per_notch, 0);
      if (lines_per_notch == 0) lines_per_notch = 3;
      double notches = (double)delta_raw / (double)WHEEL_DELTA;
      GridWheelInput in;
      in.precise        = true;   // already in px; do not re-multiply by row_h
      in.delta_px       = notches * (double)lines_per_notch * (double)cfg.row_h;
      // No phase / momentum on Win32; leave the bools false. grid_scroll_wheel
      // treats the kinetics as already-released so an overscroll immediately
      // triggers a spring-back.
      GridWheelAction act = grid_scroll_wheel(m, vp, cfg.row_h, in);
      if (act.changed) grid_repaint_w32(wd);
      if (wd.hwnd && act.start_bounce)
        SetTimer(wd.hwnd, GRID_BOUNCE_TIMER_ID, 16, nullptr);
      return;
    }

    // WM_CHAR - feed printable codepoints to the in-place cell editor.
    if (msg == WM_CHAR) {
      if (!m.edit.active) return;
      auto& te = m.edit.te;
      uint16_t unit = (uint16_t)wParam;
      uint32_t cp   = 0;
      if (unit >= 0xD800 && unit <= 0xDBFF) {
        // High surrogate - stash and wait for the low half.
        te.pending_high_surrogate = unit;
        return;
      }
      if (unit >= 0xDC00 && unit <= 0xDFFF) {
        if (te.pending_high_surrogate == 0) return;
        cp = 0x10000 + (((uint32_t)te.pending_high_surrogate - 0xD800) << 10)
                       + ((uint32_t)unit - 0xDC00);
        te.pending_high_surrogate = 0;
      } else {
        te.pending_high_surrogate = 0;
        cp = unit;
      }
      // Skip controls (Backspace / Tab / Enter / Esc arrive as WM_CHAR too).
      if (cp < 0x20 || cp == 0x7F) return;
      char buf[4];
      int  n = neui_detail::te_encode_utf8(cp, buf);
      neui_detail::te_insert_utf8(te.text, te.cursor, te.sel_anchor,
                                    te.overwrite, buf, n, &m.edit.history);
      grid_repaint_w32(wd);
      return;
    }

    if (msg == WM_KEYDOWN) {
      // Map VK_* to NEUI_KEY_* then call into the shared keydown logic.
      // We mirror the xpl host's GridWidget::on_keydown here directly to
      // avoid a virtual call cross-host. NEUI_KEY_* numeric values match
      // VK_* for the keys we care about so wParam can be passed through.
      uint32_t keycode = (uint32_t)wParam;
      uint32_t mods    = 0;
      if (GetKeyState(VK_SHIFT)   & 0x8000) mods |= NEUI_KMOD_SHIFT;
      if (GetKeyState(VK_CONTROL) & 0x8000) mods |= NEUI_KMOD_CTRL;
      if (GetKeyState(VK_MENU)    & 0x8000) mods |= NEUI_KMOD_ALT;

      // --- Edit-mode keys take priority over the nav switch ---
      if (m.edit.active) {
        auto& te    = m.edit.te;
        auto& hist  = m.edit.history;
        const bool shift = (mods & NEUI_KMOD_SHIFT) != 0;
        const bool ctrl  = (mods & NEUI_KMOD_CTRL)  != 0;
        switch (keycode) {
        case VK_RETURN: grid_commit_edit_w32(wd); return;
        case VK_ESCAPE: grid_cancel_edit_w32(wd); return;
        case VK_LEFT:
          neui_detail::te_move_left (te.text, te.cursor, te.sel_anchor, ctrl, shift, &hist);
          grid_repaint_w32(wd); return;
        case VK_RIGHT:
          neui_detail::te_move_right(te.text, te.cursor, te.sel_anchor, ctrl, shift, &hist);
          grid_repaint_w32(wd); return;
        case VK_HOME:
          neui_detail::te_move_home (te.text, te.cursor, te.sel_anchor, shift, &hist);
          grid_repaint_w32(wd); return;
        case VK_END:
          neui_detail::te_move_end  (te.text, te.cursor, te.sel_anchor, shift, &hist);
          grid_repaint_w32(wd); return;
        case VK_BACK:
          neui_detail::te_backspace     (te.text, te.cursor, te.sel_anchor, ctrl, &hist);
          grid_repaint_w32(wd); return;
        case VK_DELETE:
          neui_detail::te_delete_forward(te.text, te.cursor, te.sel_anchor, ctrl, &hist);
          grid_repaint_w32(wd); return;
        case 'A':
          if (ctrl) {
            neui_detail::te_select_all(te.text, te.cursor, te.sel_anchor, &hist);
            grid_repaint_w32(wd);
          }
          return;
        case 'C':
          if (ctrl) {
            std::string sel = neui_detail::te_selected_text(te.text, te.cursor, te.sel_anchor);
            if (!sel.empty())
              neui_detail::clipboard_set_text_win32(sel.c_str(), (uint32_t)sel.size());
          }
          return;
        case 'X':
          if (ctrl) {
            std::string sel = neui_detail::te_selected_text(te.text, te.cursor, te.sel_anchor);
            if (!sel.empty()) {
              neui_detail::clipboard_set_text_win32(sel.c_str(), (uint32_t)sel.size());
              hist.mark(neui_detail::EditState{ te.text, te.cursor, te.sel_anchor },
                        neui_detail::EditHistory::None, true);
              neui_detail::te_erase_selection(te.text, te.cursor, te.sel_anchor);
              grid_repaint_w32(wd);
            }
          }
          return;
        case 'V':
          if (ctrl) {
            int n = neui_detail::clipboard_get_text_win32(nullptr, 0);
            if (n > 0) {
              std::vector<char> buf((size_t)n);
              neui_detail::clipboard_get_text_win32(buf.data(), n);
              std::string paste(buf.data(), (size_t)(n > 0 ? n - 1 : 0));
              neui_detail::te_paste(te.text, te.cursor, te.sel_anchor, paste,
                                      /*strip_newlines=*/true, &hist);
              grid_repaint_w32(wd);
            }
          }
          return;
        case 'Z':
          if (ctrl) {
            if (shift) neui_detail::te_redo(te.text, te.cursor, te.sel_anchor, hist);
            else       neui_detail::te_undo(te.text, te.cursor, te.sel_anchor, hist);
            grid_repaint_w32(wd);
          }
          return;
        case 'Y':
          if (ctrl) {
            neui_detail::te_redo(te.text, te.cursor, te.sel_anchor, hist);
            grid_repaint_w32(wd);
          }
          return;
        default:
          return;  // swallow other keys while editing
        }
      }

      int n_rows = (int)m.rows.size();
      int n_cols = (int)m.columns.size();
      if (n_rows == 0) return;

      // Navigation walks visual order so Up / Down move the cursor through
      // the rows the user sees after sorting. grid_set_selected_visual then
      // stores the corresponding logical row into selected_row.
      grid_ensure_sort_clean(m);

      int prev_row = m.selected_row;
      int prev_col = m.selected_col;
      int vis = grid_visible_rows(vp, cfg.row_h);
      if (vis < 1) vis = 1;
      bool handled = true;
      switch (keycode) {
      case VK_UP: {
        int v = grid_selected_visual(m);
        grid_set_selected_visual(m, (v < 0) ? 0 : (v - 1));
        break;
      }
      case VK_DOWN: {
        int v = grid_selected_visual(m);
        grid_set_selected_visual(m, (v < 0) ? 0 : (v + 1));
        break;
      }
      case VK_PRIOR: {
        int v = grid_selected_visual(m);
        grid_set_selected_visual(m, (v < 0) ? 0 : (v - vis));
        break;
      }
      case VK_NEXT: {
        int v = grid_selected_visual(m);
        grid_set_selected_visual(m, (v < 0) ? vis : (v + vis));
        break;
      }
      case VK_HOME:
        if (cfg.cell_focus && !(mods & NEUI_KMOD_CTRL)) {
          m.selected_col = (n_cols > 0) ? 0 : -1;
          if (m.selected_row < 0) grid_set_selected_visual(m, 0);
        } else {
          grid_set_selected_visual(m, 0);
          if (cfg.cell_focus) m.selected_col = (n_cols > 0) ? 0 : -1;
        }
        break;
      case VK_END:
        if (cfg.cell_focus && !(mods & NEUI_KMOD_CTRL)) {
          m.selected_col = (n_cols > 0) ? n_cols - 1 : -1;
          if (m.selected_row < 0) grid_set_selected_visual(m, n_rows - 1);
        } else {
          grid_set_selected_visual(m, n_rows - 1);
          if (cfg.cell_focus) m.selected_col = (n_cols > 0) ? n_cols - 1 : -1;
        }
        break;
      case VK_LEFT:
        if (cfg.cell_focus) {
          if (m.selected_col > 0) m.selected_col--;
          else if (m.selected_col < 0 && n_cols > 0) m.selected_col = 0;
          if (m.selected_row < 0) m.selected_row = 0;
        } else {
          m.scroll_offset_x -= grid_horizontal_step_px(m);
          grid_clamp_scroll(m, vp, cfg.row_h);
          grid_repaint_w32(wd);
          return;
        }
        break;
      case VK_RIGHT:
        if (cfg.cell_focus) {
          if (m.selected_col < n_cols - 1) {
            if (m.selected_col < 0) m.selected_col = 0;
            else                     m.selected_col++;
          }
          if (m.selected_row < 0) m.selected_row = 0;
        } else {
          m.scroll_offset_x += grid_horizontal_step_px(m);
          grid_clamp_scroll(m, vp, cfg.row_h);
          grid_repaint_w32(wd);
          return;
        }
        break;
      case VK_RETURN: {
        int r = m.selected_row;
        if (r < 0) return;
        // Cell-edit takes priority over ROW_ACTIVATED when the column is
        // editable and we're in cell-focus mode.
        if (cfg.cell_focus && m.selected_col >= 0 &&
            grid_try_begin_edit_w32(wd, r, m.selected_col)) {
          return;
        }
        grid_fire_row_activated_w32(wd, r);
        return;
      }
      default:
        handled = false; break;
      }
      if (!handled) return;
      if (cfg.cell_focus && m.selected_col >= 0)
        grid_ensure_cell_visible(m, vp, cfg.row_h, m.selected_row, m.selected_col);
      else
        grid_ensure_row_visible(m, vp, cfg.row_h, m.selected_row);
      if (m.selected_row != prev_row)
        grid_fire_row_selected_w32(wd, m.selected_row);
      if (cfg.cell_focus &&
          (m.selected_row != prev_row || m.selected_col != prev_col))
        grid_fire_cell_selected_w32(wd, m.selected_row, m.selected_col);
      grid_repaint_w32(wd);
      return;
    }

    // Mouse coordinates in logical pixels, HWND-local.
    int phys_x = GET_X_LPARAM(lParam);
    int phys_y = GET_Y_LPARAM(lParam);
    int lx = phys_to_log_w32(phys_x, dpi);
    int ly = phys_to_log_w32(phys_y, dpi);

    // --- column-resize drag in progress ---
    if (m.column_resize_col >= 0) {
      if (msg == WM_MOUSEMOVE) {
        int dx = phys_x - m.column_resize_start_x;
        // Convert pixel delta to logical pixels before adding to width.
        int dx_log = phys_to_log_w32(dx, dpi);
        int new_w  = m.column_resize_start_w + dx_log;
        int min_w  = grid_column_min_width(m, m.column_resize_col,
                                              cfg.col_min_w_def);
        if (new_w < min_w) new_w = min_w;
        if (new_w > 5000)  new_w = 5000;
        m.columns[(size_t)m.column_resize_col].width = new_w;
        grid_clamp_scroll(m, vp, cfg.row_h);
        platform_set_cursor_w32(NEUI_CURSOR_EW_RESIZE);
        grid_repaint_w32(wd);
        return;
      }
      if (msg == WM_LBUTTONUP) {
        int new_w = m.columns[(size_t)m.column_resize_col].width;
        int col   = m.column_resize_col;
        int old_w = m.column_resize_old_w;
        m.column_resize_col = -1;
        platform_set_cursor_w32(NEUI_CURSOR_DEFAULT);
        if (new_w != old_w) grid_fire_column_resized_w32(wd, col, old_w, new_w);
        grid_repaint_w32(wd);
        return;
      }
      return;
    }

    // --- vertical scrollbar drag in progress ---
    if (m.vert_drag.active) {
      if (msg == WM_LBUTTONUP ||
          (msg == WM_MOUSEMOVE && !(wParam & MK_LBUTTON))) {
        m.vert_drag.active = false;
        return;
      }
      if (msg == WM_MOUSEMOVE) {
        int vis = grid_visible_rows(vp, cfg.row_h);
        ScrollbarGeom g = compute_scrollbar(vp.body_h, 0,
                                              (int)m.rows.size(), vis,
                                              m.vert_drag.start_position);
        int rel = ly - vp.body_y;
        m.scroll_offset_y = scrollbar_drag_apply(m.vert_drag, rel, g,
                                                    (int)m.rows.size(), vis);
        grid_clamp_scroll(m, vp, cfg.row_h);
        grid_repaint_w32(wd);
        return;
      }
      return;
    }

    // --- horizontal scrollbar drag in progress ---
    if (m.horz_drag.active) {
      if (msg == WM_LBUTTONUP ||
          (msg == WM_MOUSEMOVE && !(wParam & MK_LBUTTON))) {
        m.horz_drag.active = false;
        return;
      }
      if (msg == WM_MOUSEMOVE) {
        int content_w = grid_total_content_width(m);
        ScrollbarGeom g = compute_scrollbar(vp.body_w, 0,
                                              content_w, vp.body_w,
                                              m.horz_drag.start_position);
        int rel = lx - vp.body_x;
        m.scroll_offset_x = scrollbar_drag_apply(m.horz_drag, rel, g,
                                                   content_w, vp.body_w);
        grid_clamp_scroll(m, vp, cfg.row_h);
        grid_repaint_w32(wd);
        return;
      }
      return;
    }

    // --- mouse move: cursor feedback for header divider ---
    if (msg == WM_MOUSEMOVE) {
      GridHit hit = grid_hit_test(m, vp, cfg.row_h,
                                    widget_w, widget_h, lx, ly);
      platform_set_cursor_w32(hit.region == GridHitRegion::HeaderDivider
                                ? NEUI_CURSOR_EW_RESIZE
                                : NEUI_CURSOR_DEFAULT);
      return;
    }

    // --- button down / dbl-click ---
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
      // Always take focus so subsequent key events route here.
      if (wd.hwnd) SetFocus(wd.hwnd);
      // Hit-test reads display_order, so rebuild it first if dirty.
      grid_ensure_sort_clean(m);
      GridHit hit = grid_hit_test(m, vp, cfg.row_h, widget_w, widget_h, lx, ly);

      // --- Edit-mode click handling ---
      // Click inside the editing cell: swallow (the editor stays open).
      // Click anywhere else: commit. If the commit is rejected by the
      // validate callback, swallow the click so the underlying grid
      // doesn't also act on it (editor stays open with the proposed text).
      if (m.edit.active) {
        bool on_editing_cell = (hit.region == GridHitRegion::Cell &&
                                hit.row == m.edit.row &&
                                hit.col == m.edit.col);
        if (on_editing_cell) return;
        if (!grid_commit_edit_w32(wd)) return;
        // Commit succeeded - editor closed. Fall through.
      }
      switch (hit.region) {
      case GridHitRegion::HeaderDivider:
        if (msg == WM_LBUTTONDOWN) {
          m.column_resize_col     = hit.col;
          m.column_resize_start_x = phys_x;     // store physical for delta math
          m.column_resize_start_w = m.columns[(size_t)hit.col].width;
          m.column_resize_old_w   = m.column_resize_start_w;
          platform_set_cursor_w32(NEUI_CURSOR_EW_RESIZE);
        }
        return;
      case GridHitRegion::Header:
        // Header click cycles the sort for a sortable column. Shift = add /
        // cycle a secondary level; plain click replaces the stack.
        if (msg == WM_LBUTTONDOWN && grid_header_click_allowed(m, hit.col)) {
          bool shift = (wParam & MK_SHIFT) != 0;
          neui_grid_sort_dir_t new_dir =
            grid_apply_header_click(m, hit.col, shift);
          grid_repaint_w32(wd);
          grid_fire_sort_changed_w32(wd, hit.col, new_dir);
        }
        return;
      case GridHitRegion::VertScrollTrack: {
        int vis = grid_visible_rows(vp, cfg.row_h);
        ScrollbarGeom g = compute_scrollbar(vp.body_h, 0,
                                              (int)m.rows.size(), vis,
                                              m.scroll_offset_y);
        int rel = ly - vp.body_y;
        if (g.visible && rel >= g.thumb_pos && rel < g.thumb_pos + g.thumb_len) {
          m.vert_drag.active           = true;
          m.vert_drag.start_axis_coord = rel;
          m.vert_drag.start_position   = m.scroll_offset_y;
        } else if (g.visible) {
          int step = vis > 0 ? vis : 1;
          if (rel < g.thumb_pos) m.scroll_offset_y -= step;
          else                   m.scroll_offset_y += step;
          grid_clamp_scroll(m, vp, cfg.row_h);
          grid_repaint_w32(wd);
        }
        return;
      }
      case GridHitRegion::HorzScrollTrack: {
        int content_w = grid_total_content_width(m);
        ScrollbarGeom g = compute_scrollbar(vp.body_w, 0,
                                              content_w, vp.body_w,
                                              m.scroll_offset_x);
        int rel = lx - vp.body_x;
        if (g.visible && rel >= g.thumb_pos && rel < g.thumb_pos + g.thumb_len) {
          m.horz_drag.active           = true;
          m.horz_drag.start_axis_coord = rel;
          m.horz_drag.start_position   = m.scroll_offset_x;
        } else if (g.visible) {
          int step = vp.body_w > 0 ? vp.body_w : 60;
          if (rel < g.thumb_pos) m.scroll_offset_x -= step;
          else                   m.scroll_offset_x += step;
          grid_clamp_scroll(m, vp, cfg.row_h);
          grid_repaint_w32(wd);
        }
        return;
      }
      case GridHitRegion::Cell: {
        if (msg == WM_LBUTTONDBLCLK) {
          // Try opening the in-place editor first (mirrors ENTER); falls
          // back to ROW_ACTIVATED for non-editable cells / row-focus mode.
          if (!grid_try_begin_edit_w32(wd, hit.row, hit.col))
            grid_fire_row_activated_w32(wd, hit.row);
        } else {
          const GridCellOverride* ov = grid_find_override(m, hit.row, hit.col);
          bool cell_dis = ov && ov->has_enabled && !ov->enabled;
          int prev_row = m.selected_row;
          m.selected_row = hit.row;
          if (cfg.cell_focus) m.selected_col = hit.col;
          if (cell_dis) {
            if (m.selected_row != prev_row)
              grid_fire_row_selected_w32(wd, hit.row);
          } else {
            grid_click_ladder_w32(wd, hit.row, hit.col);
          }
        }
        grid_repaint_w32(wd);
        return;
      }
      case GridHitRegion::BodyEmpty:
        if (m.selected_row != -1) {
          m.selected_row = -1;
          m.selected_col = -1;
          grid_fire_row_selected_w32(wd, -1);
          grid_repaint_w32(wd);
        }
        return;
      default:
        return;
      }
    }

    if (msg == WM_LBUTTONUP) {
      // Stray UP (e.g. drag started outside) - reset transient state.
      m.vert_drag.active = false;
      m.horz_drag.active = false;
      return;
    }
  }

  // -------------------------------------------------------------------------
  // Grid API (NEUI_API_GRID) - thin wrappers over WidgetData::grid_model.

  static WidgetData* resolve_grid_w32(neui_session_t session, neui_widget_t widget,
                                        Session** out_sess = nullptr)
  {
    auto* s = get_session_for_widget(session, widget);
    if (out_sess) *out_sess = s;
    if (!s) return nullptr;
    uint32_t idx = WidgetToIndex(widget);
    WidgetData* wd = s->get_widget(idx);
    if (!wd || !wd->type || strcmp(wd->type, NEUI_W_GRID) != 0) return nullptr;
    return wd;
  }

  static void grid_invalidate_w32(WidgetData* wd)
  {
    if (wd && wd->hwnd) InvalidateRect(wd->hwnd, nullptr, FALSE);
  }

  static int NEUI_ABI gr_add_column(neui_session_t session, neui_widget_t widget,
                                      const char* header, int width_logical)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd) return -1;
    auto& m = ensure_grid_model_w32(*wd);
    neui_detail::GridColumn c;
    c.header = header ? header : "";
    c.width  = (width_logical > 0) ? width_logical : neui_detail::GRID_DEFAULT_NEW_COLUMN_W;
    m.columns.push_back(std::move(c));
    neui_detail::grid_resize_rows_to_columns(m, (int)m.columns.size());
    grid_invalidate_w32(wd);
    return (int)m.columns.size() - 1;
  }

  static int NEUI_ABI gr_get_column_count(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_w32(session, widget);
    return wd && wd->grid_model ? (int)wd->grid_model->columns.size() : 0;
  }

  static void NEUI_ABI gr_set_column_width(neui_session_t session, neui_widget_t widget,
                                             int col, int width_logical)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return;
    auto cfg = neui_detail::grid_read_config(wd->attrs.get());
    int min_w = neui_detail::grid_column_min_width(m, col, cfg.col_min_w_def);
    if (width_logical < min_w) width_logical = min_w;
    m.columns[(size_t)col].width = width_logical;
    grid_invalidate_w32(wd);
  }

  static int NEUI_ABI gr_get_column_width(neui_session_t session, neui_widget_t widget, int col)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd || !wd->grid_model) return 0;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return 0;
    return m.columns[(size_t)col].width;
  }

  static void NEUI_ABI gr_set_column_min_width(neui_session_t session, neui_widget_t widget,
                                                  int col, int min_w)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return;
    m.columns[(size_t)col].min_width = min_w;
    if (m.columns[(size_t)col].width < min_w)
      m.columns[(size_t)col].width = min_w;
    grid_invalidate_w32(wd);
  }

  static void NEUI_ABI gr_set_column_align(neui_session_t session, neui_widget_t widget,
                                             int col, const char* align)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return;
    m.columns[(size_t)col].align = neui_detail::grid_parse_align(align);
    grid_invalidate_w32(wd);
  }

  static void NEUI_ABI gr_set_column_header(neui_session_t session, neui_widget_t widget,
                                              int col, const char* text)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return;
    m.columns[(size_t)col].header = text ? text : "";
    grid_invalidate_w32(wd);
  }

  static int NEUI_ABI gr_get_column_header(neui_session_t session, neui_widget_t widget,
                                             int col, char* buf, int buflen)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd || !wd->grid_model) return -1;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return -1;
    const std::string& h = m.columns[(size_t)col].header;
    int need = (int)h.size() + 1;
    if (buf && buflen > 0) {
      int copy = (need < buflen) ? need : buflen;
      memcpy(buf, h.c_str(), (size_t)copy);
      buf[copy - 1] = 0;
    }
    return need;
  }

  static void NEUI_ABI gr_remove_column(neui_session_t session, neui_widget_t widget, int col)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return;
    m.columns.erase(m.columns.begin() + col);
    for (auto& r : m.rows)
      if (col < (int)r.cells.size()) r.cells.erase(r.cells.begin() + col);
    std::unordered_map<uint64_t, neui_detail::GridCellOverride> remap;
    for (auto& kv : m.cell_overrides) {
      int r = (int)(kv.first >> 32);
      int c = (int)(kv.first & 0xFFFFFFFF);
      if (c == col) continue;
      int nc = (c > col) ? c - 1 : c;
      remap[neui_detail::grid_cell_key(r, nc)] = kv.second;
    }
    m.cell_overrides = std::move(remap);
    if (m.selected_col >= (int)m.columns.size())
      m.selected_col = (int)m.columns.size() - 1;
    // Sort levels referencing the removed column disappear; later columns
    // shift index down by one. Mark dirty for the rebuild.
    neui_detail::grid_sort_on_column_removed(m, col);
    grid_invalidate_w32(wd);
  }

  static void NEUI_ABI gr_clear_columns(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_w32(*wd);
    m.columns.clear();
    m.rows.clear();
    m.cell_overrides.clear();
    m.selected_row = -1;
    m.selected_col = -1;
    m.scroll_offset_x = 0;
    m.scroll_offset_y = 0;
    m.sort_stack.clear();
    m.display_order.clear();
    m.logical_to_visual.clear();
    m.sort_dirty = false;
    grid_invalidate_w32(wd);
  }

  static int NEUI_ABI gr_add_row(neui_session_t session, neui_widget_t widget,
                                   const char* const* values_utf8)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd) return -1;
    auto& m = ensure_grid_model_w32(*wd);
    neui_detail::GridRow row;
    row.cells.resize(m.columns.size());
    if (values_utf8) {
      for (size_t i = 0; i < m.columns.size() && values_utf8[i]; ++i)
        row.cells[i] = values_utf8[i];
    }
    m.rows.push_back(std::move(row));
    m.sort_dirty = true;   // bulk insert collapses to one rebuild at next paint
    grid_invalidate_w32(wd);
    return (int)m.rows.size() - 1;
  }

  static int NEUI_ABI gr_get_row_count(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_w32(session, widget);
    return wd && wd->grid_model ? (int)wd->grid_model->rows.size() : 0;
  }

  static void NEUI_ABI gr_remove_row(neui_session_t session, neui_widget_t widget, int row)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (row < 0 || row >= (int)m.rows.size()) return;
    m.rows.erase(m.rows.begin() + row);
    std::unordered_map<uint64_t, neui_detail::GridCellOverride> remap;
    for (auto& kv : m.cell_overrides) {
      int r = (int)(kv.first >> 32);
      int c = (int)(kv.first & 0xFFFFFFFF);
      if (r == row) continue;
      int nr = (r > row) ? r - 1 : r;
      remap[neui_detail::grid_cell_key(nr, c)] = kv.second;
    }
    m.cell_overrides = std::move(remap);
    if (m.selected_row >= (int)m.rows.size())
      m.selected_row = (int)m.rows.size() - 1;
    m.sort_dirty = true;
    grid_invalidate_w32(wd);
  }

  static void NEUI_ABI gr_clear_rows(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    m.rows.clear();
    m.cell_overrides.clear();
    m.selected_row = -1;
    m.scroll_offset_y = 0;
    m.display_order.clear();
    m.logical_to_visual.clear();
    m.sort_dirty = false;  // empty rows means nothing to sort
    grid_invalidate_w32(wd);
  }

  static void NEUI_ABI gr_set_cell_text(neui_session_t session, neui_widget_t widget,
                                          int row, int col, const char* utf8)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (row < 0 || row >= (int)m.rows.size()) return;
    if (col < 0 || col >= (int)m.columns.size()) return;
    auto& r = m.rows[(size_t)row];
    if ((int)r.cells.size() <= col) r.cells.resize((size_t)col + 1);
    r.cells[(size_t)col] = utf8 ? utf8 : "";
    // Only the sorted column's text changes the order; checking is cheap so
    // mark dirty unconditionally and let the lazy rebuild bail when nothing
    // sorted changed.
    m.sort_dirty = true;
    grid_invalidate_w32(wd);
  }

  static int NEUI_ABI gr_get_cell_text(neui_session_t session, neui_widget_t widget,
                                         int row, int col, char* buf, int buflen)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd || !wd->grid_model) return -1;
    auto& m = *wd->grid_model;
    if (row < 0 || row >= (int)m.rows.size()) return -1;
    if (col < 0 || col >= (int)m.columns.size()) return -1;
    const auto& r = m.rows[(size_t)row];
    static const std::string empty;
    const std::string& src = (col < (int)r.cells.size()) ? r.cells[(size_t)col] : empty;
    int need = (int)src.size() + 1;
    if (buf && buflen > 0) {
      int copy = (need < buflen) ? need : buflen;
      memcpy(buf, src.c_str(), (size_t)copy);
      buf[copy - 1] = 0;
    }
    return need;
  }

  static void NEUI_ABI gr_set_cell_color(neui_session_t session, neui_widget_t widget,
                                           int row, int col, uint32_t argb)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (row < 0 || row >= (int)m.rows.size()) return;
    if (col < 0 || col >= (int)m.columns.size()) return;
    if (argb == 0) {
      auto* ov = neui_detail::grid_find_override(m, row, col);
      if (ov) {
        ov->has_color = false;
        ov->color     = 0;
        neui_detail::grid_prune_override(m, row, col);
      }
    } else {
      auto& ov = neui_detail::grid_ensure_override(m, row, col);
      ov.color     = argb;
      ov.has_color = true;
    }
    grid_invalidate_w32(wd);
  }

  static void NEUI_ABI gr_set_cell_enabled(neui_session_t session, neui_widget_t widget,
                                             int row, int col, bool enabled)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (row < 0 || row >= (int)m.rows.size()) return;
    if (col < 0 || col >= (int)m.columns.size()) return;
    auto& ov = neui_detail::grid_ensure_override(m, row, col);
    ov.enabled     = enabled;
    ov.has_enabled = true;
    if (enabled && !ov.has_color) {
      ov.has_enabled = false;
      neui_detail::grid_prune_override(m, row, col);
    }
    grid_invalidate_w32(wd);
  }

  static void NEUI_ABI gr_clear_cell_overrides(neui_session_t session, neui_widget_t widget,
                                                  int row, int col)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd || !wd->grid_model) return;
    wd->grid_model->cell_overrides.erase(neui_detail::grid_cell_key(row, col));
    grid_invalidate_w32(wd);
  }

  static void NEUI_ABI gr_set_selected_row(neui_session_t session, neui_widget_t widget, int row)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_w32(*wd);
    int n = (int)m.rows.size();
    if (row < -1)  row = -1;
    if (row >= n)  row = n - 1;
    m.selected_row = row;
    auto cfg = neui_detail::grid_read_config(wd->attrs.get());
    if (cfg.cell_focus && m.selected_col < 0 && !m.columns.empty())
      m.selected_col = 0;
    if (row >= 0) {
      UINT dpi = 0;
      auto vp = grid_viewport_w32(*wd, &dpi);
      neui_detail::grid_ensure_row_visible(m, vp, cfg.row_h, row);
    }
    grid_invalidate_w32(wd);
  }

  static int NEUI_ABI gr_get_selected_row(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_w32(session, widget);
    return wd && wd->grid_model ? wd->grid_model->selected_row : -1;
  }

  static void NEUI_ABI gr_set_selected_cell(neui_session_t session, neui_widget_t widget,
                                              int row, int col)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_w32(*wd);
    int n_rows = (int)m.rows.size();
    int n_cols = (int)m.columns.size();
    if (row < -1)       row = -1;
    if (row >= n_rows)  row = n_rows - 1;
    if (col < -1)       col = -1;
    if (col >= n_cols)  col = n_cols - 1;
    m.selected_row = row;
    m.selected_col = col;
    if (row >= 0 && col >= 0) {
      auto cfg = neui_detail::grid_read_config(wd->attrs.get());
      auto vp  = grid_viewport_w32(*wd);
      neui_detail::grid_ensure_cell_visible(m, vp, cfg.row_h, row, col);
    }
    grid_invalidate_w32(wd);
  }

  static void NEUI_ABI gr_get_selected_cell(neui_session_t session, neui_widget_t widget,
                                              int* out_row, int* out_col)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (out_row) *out_row = (wd && wd->grid_model) ? wd->grid_model->selected_row : -1;
    if (out_col) {
      if (!wd || !wd->grid_model) { *out_col = -1; return; }
      auto cfg = neui_detail::grid_read_config(wd->attrs.get());
      *out_col = cfg.cell_focus ? wd->grid_model->selected_col : -1;
    }
  }

  static void NEUI_ABI gr_ensure_row_visible(neui_session_t session, neui_widget_t widget, int row)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_w32(*wd);
    auto cfg = neui_detail::grid_read_config(wd->attrs.get());
    auto vp  = grid_viewport_w32(*wd);
    neui_detail::grid_ensure_row_visible(m, vp, cfg.row_h, row);
    grid_invalidate_w32(wd);
  }

  static void NEUI_ABI gr_ensure_cell_visible(neui_session_t session, neui_widget_t widget,
                                                int row, int col)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_w32(*wd);
    auto cfg = neui_detail::grid_read_config(wd->attrs.get());
    auto vp  = grid_viewport_w32(*wd);
    neui_detail::grid_ensure_cell_visible(m, vp, cfg.row_h, row, col);
    grid_invalidate_w32(wd);
  }

  static void NEUI_ABI gr_set_scroll_x(neui_session_t session, neui_widget_t widget, int x)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_w32(*wd);
    m.scroll_offset_x = x;
    auto cfg = neui_detail::grid_read_config(wd->attrs.get());
    auto vp  = grid_viewport_w32(*wd);
    neui_detail::grid_clamp_scroll(m, vp, cfg.row_h);
    grid_invalidate_w32(wd);
  }

  static int NEUI_ABI gr_get_scroll_x(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_w32(session, widget);
    return wd && wd->grid_model ? wd->grid_model->scroll_offset_x : 0;
  }

  static int NEUI_ABI gr_hit_test(neui_session_t session, neui_widget_t widget,
                                    int lx, int ly, int* out_row, int* out_col)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (out_row) *out_row = -1;
    if (out_col) *out_col = -1;
    if (!wd || !wd->grid_model) return 0;
    auto& m = *wd->grid_model;
    auto cfg = neui_detail::grid_read_config(wd->attrs.get());
    auto vp  = grid_viewport_w32(*wd);
    RECT rc{}; if (wd->hwnd) GetClientRect(wd->hwnd, &rc);
    UINT dpi = wd->session ? wd->session->get_dpi_for_widget(wd->index) : 96;
    if (dpi == 0) dpi = 96;
    int widget_w = phys_to_log_w32(rc.right,  dpi);
    int widget_h = phys_to_log_w32(rc.bottom, dpi);
    auto hit = neui_detail::grid_hit_test(m, vp, cfg.row_h,
                                            widget_w, widget_h, lx, ly);
    if (hit.region != neui_detail::GridHitRegion::Cell) return 0;
    if (out_row) *out_row = hit.row;
    if (out_col) *out_col = hit.col;
    return 1;
  }

  // -------- Sort API ----------------------------------------------------

  static void NEUI_ABI gr_set_column_sortable(neui_session_t session, neui_widget_t widget,
                                                int col, bool sortable)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return;
    m.columns[(size_t)col].sortable = sortable;
    // Clearing the sortable flag does NOT auto-remove an existing sort on
    // that column - programmatic set_sort / add_sort should still work. Only
    // user-driven header clicks are gated by this flag.
  }

  static void NEUI_ABI gr_set_column_sort_kind(neui_session_t session, neui_widget_t widget,
                                                 int col, neui_grid_sort_kind_t kind)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return;
    m.columns[(size_t)col].sort_kind = kind;
    // If this column is in the active sort the next paint should re-sort.
    if (neui_detail::grid_sort_stack_find(m, col) >= 0) {
      m.sort_dirty = true;
      grid_invalidate_w32(wd);
    }
  }

  static void NEUI_ABI gr_set_sort(neui_session_t session, neui_widget_t widget,
                                     int col, neui_grid_sort_dir_t dir)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_w32(*wd);
    neui_detail::grid_set_sort(m, col, dir);
    grid_invalidate_w32(wd);
  }

  static void NEUI_ABI gr_add_sort(neui_session_t session, neui_widget_t widget,
                                     int col, neui_grid_sort_dir_t dir)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_w32(*wd);
    neui_detail::grid_add_sort(m, col, dir);
    grid_invalidate_w32(wd);
  }

  static void NEUI_ABI gr_clear_sort(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_w32(*wd);
    neui_detail::grid_clear_sort(m);
    grid_invalidate_w32(wd);
  }

  static int NEUI_ABI gr_get_sort_count(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_w32(session, widget);
    return (wd && wd->grid_model) ? (int)wd->grid_model->sort_stack.size() : 0;
  }

  static void NEUI_ABI gr_get_sort_level(neui_session_t session, neui_widget_t widget,
                                           int level, int* out_col,
                                           neui_grid_sort_dir_t* out_dir)
  {
    if (out_col) *out_col = -1;
    if (out_dir) *out_dir = NEUI_GRID_SORT_NONE;
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (level < 0 || level >= (int)m.sort_stack.size()) return;
    if (out_col) *out_col = m.sort_stack[(size_t)level].col;
    if (out_dir) *out_dir = m.sort_stack[(size_t)level].dir;
  }

  static int NEUI_ABI gr_logical_to_visual_row(neui_session_t session, neui_widget_t widget,
                                                  int logical_row)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd || !wd->grid_model) return -1;
    auto& m = *wd->grid_model;
    if (logical_row < 0 || logical_row >= (int)m.rows.size()) return -1;
    neui_detail::grid_ensure_sort_clean(m);
    return neui_detail::grid_logical_to_visual(m, logical_row);
  }

  static int NEUI_ABI gr_visual_to_logical_row(neui_session_t session, neui_widget_t widget,
                                                  int visual_row)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd || !wd->grid_model) return -1;
    auto& m = *wd->grid_model;
    if (visual_row < 0 || visual_row >= (int)m.rows.size()) return -1;
    neui_detail::grid_ensure_sort_clean(m);
    return neui_detail::grid_visual_to_logical(m, visual_row);
  }

  // -------- Cell editing API (win32) ------------------------------------

  static void NEUI_ABI gr_set_column_editable(neui_session_t session, neui_widget_t widget,
                                                int col, bool editable)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_w32(*wd);
    if (col < 0 || col >= (int)m.columns.size()) return;
    m.columns[(size_t)col].editable = editable;
    if (!editable && m.edit.active && m.edit.col == col)
      grid_cancel_edit_w32(*wd);
  }

  static bool NEUI_ABI gr_get_column_editable(neui_session_t session, neui_widget_t widget,
                                                int col)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd || !wd->grid_model) return false;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return false;
    return m.columns[(size_t)col].editable;
  }

  static void NEUI_ABI gr_begin_cell_edit(neui_session_t session, neui_widget_t widget,
                                           int row, int col)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd) return;
    if (grid_try_begin_edit_w32(*wd, row, col) && wd->hwnd) SetFocus(wd->hwnd);
  }

  static void NEUI_ABI gr_end_cell_edit(neui_session_t session, neui_widget_t widget,
                                         bool commit)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd || !wd->grid_model || !wd->grid_model->edit.active) return;
    if (commit) (void)grid_commit_edit_w32(*wd);
    else        grid_cancel_edit_w32(*wd);
  }

  static bool NEUI_ABI gr_is_editing_cell(neui_session_t session, neui_widget_t widget,
                                            int* out_row, int* out_col)
  {
    auto* wd = resolve_grid_w32(session, widget);
    if (!wd || !wd->grid_model || !wd->grid_model->edit.active) {
      if (out_row) *out_row = -1;
      if (out_col) *out_col = -1;
      return false;
    }
    auto& m = *wd->grid_model;
    if (out_row) *out_row = m.edit.row;
    if (out_col) *out_col = m.edit.col;
    return true;
  }

  neui_grid_api_t grid_api = {
    NEUI_VERSION,
    gr_add_column,
    gr_get_column_count,
    gr_set_column_width,
    gr_get_column_width,
    gr_set_column_min_width,
    gr_set_column_align,
    gr_set_column_header,
    gr_get_column_header,
    gr_remove_column,
    gr_clear_columns,
    gr_add_row,
    gr_get_row_count,
    gr_remove_row,
    gr_clear_rows,
    gr_set_cell_text,
    gr_get_cell_text,
    gr_set_cell_color,
    gr_set_cell_enabled,
    gr_clear_cell_overrides,
    gr_set_selected_row,
    gr_get_selected_row,
    gr_set_selected_cell,
    gr_get_selected_cell,
    gr_ensure_row_visible,
    gr_ensure_cell_visible,
    gr_set_scroll_x,
    gr_get_scroll_x,
    gr_hit_test,
    gr_set_column_sortable,
    gr_set_column_sort_kind,
    gr_set_sort,
    gr_add_sort,
    gr_clear_sort,
    gr_get_sort_count,
    gr_get_sort_level,
    gr_logical_to_visual_row,
    gr_visual_to_logical_row,
    gr_set_column_editable,
    gr_get_column_editable,
    gr_begin_cell_edit,
    gr_end_cell_edit,
    gr_is_editing_cell,
  };

  // -------------------------------------------------------------------------
  // DnD API (NEUI_API_DND). Drop-target only in v1. On the win32 native
  // host descendants of a frame are HWND-backed; the framework only
  // reports drops to widgets that have `drop_target=true`. v1 typically
  // means the frame widget itself, since child HWNDs do not register as
  // OLE drop targets.

  static void NEUI_ABI dnd_set_drop_target(neui_session_t session,
                                            neui_widget_t widget, bool enable)
  {
    auto* s = get_session(session);
    if (!s) return;
    auto* wd = s->get_widget(WidgetToIndex(widget));
    if (!wd) return;
    wd->drop_target = enable;
  }

  static bool NEUI_ABI dnd_get_drop_target(neui_session_t session,
                                            neui_widget_t widget)
  {
    auto* s = get_session(session);
    if (!s) return false;
    auto* wd = s->get_widget(WidgetToIndex(widget));
    return wd && wd->drop_target;
  }

  static void NEUI_ABI dnd_set_accepted_formats(neui_session_t session,
                                                 neui_widget_t widget,
                                                 const char* const* mimes,
                                                 int count)
  {
    auto* s = get_session(session);
    if (!s) return;
    auto* wd = s->get_widget(WidgetToIndex(widget));
    if (!wd) return;
    wd->accepted_mimes.clear();
    if (mimes && count > 0) {
      wd->accepted_mimes.reserve(static_cast<size_t>(count));
      for (int i = 0; i < count; ++i) {
        if (mimes[i]) wd->accepted_mimes.emplace_back(mimes[i]);
      }
    }
  }

  static void NEUI_ABI dnd_accept(neui_session_t session,
                                   neui_dnd_action_t action)
  {
    auto* s = get_session(session);
    if (!s) return;
    if (!s->_in_dnd_dispatch) return;
    s->_last_accepted_action = static_cast<uint32_t>(action);
  }

  // Internal worker shared by both public entry points. `preview_asset` and
  // hot-spot may be all zero / -1 for "no preview".
  static neui_dnd_action_t dnd_begin_drag_impl(neui_session_t session,
                                                 neui_widget_t source_widget,
                                                 neui_data_item_t payload,
                                                 uint32_t allowed_actions,
                                                 neui_asset_t preview_asset,
                                                 int hot_x, int hot_y)
  {
    auto* s = get_session_for_widget(session, source_widget);
    if (!s) return NEUI_DND_ACTION_NONE;
    if (s->_in_dnd_dispatch) return NEUI_DND_ACTION_NONE;    // no re-entry
    if (s->_drag_source_active) return NEUI_DND_ACTION_NONE; // one drag at a time
    auto* item = s->_data_items.get(payload.id);
    if (!item) return NEUI_DND_ACTION_NONE;
    HWND frame = s->find_parent_hwnd(WidgetToIndex(source_widget));
    if (!frame) {
      // Source widget itself might be the frame.
      auto* wd = s->get_widget(WidgetToIndex(source_widget));
      if (wd) frame = wd->hwnd;
    }
    if (!frame) return NEUI_DND_ACTION_NONE;

    // Resolve the preview asset to an HBITMAP if one was supplied AND it
    // belongs to this session AND it has displayable pixels. Anything that
    // doesn't pass these checks degrades gracefully to no-preview.
    neui_detail::DragPreviewW32 preview;
    bool have_preview = false;
    if (preview_asset.id != asset_none.id) {
      uint32_t a_sess = (preview_asset.id >> 16) & 0xffff;
      if (a_sess == (s->session_id() & 0xffff)) {
        const uint8_t* bgra = nullptr;
        uint32_t       w_px = 0, h_px = 0;
        float          scale = 1.0f;
        if (s->_asset_manager.get_pixels_for_export(preview_asset.id & 0xffff,
                                                      &bgra, &w_px, &h_px,
                                                      &scale)) {
          HBITMAP hbm = neui_detail::w32_make_drag_hbitmap(bgra, w_px, h_px);
          if (hbm) {
            preview.hbitmap = hbm;
            preview.width   = static_cast<int>(w_px);
            preview.height  = static_cast<int>(h_px);
            preview.hot_x   = (hot_x < 0) ? preview.width  / 2 : hot_x;
            preview.hot_y   = (hot_y < 0) ? preview.height / 2 : hot_y;
            have_preview = true;
          }
        }
      }
    }

    s->_drag_source_active = true;
    uint32_t r = neui_detail::platform_dnd_begin_drag_w32(
                    frame, item, allowed_actions,
                    have_preview ? &preview : nullptr);
    s->_drag_source_active = false;
    return static_cast<neui_dnd_action_t>(r);
  }

  static neui_dnd_action_t NEUI_ABI dnd_begin_drag(neui_session_t session,
                                                    neui_widget_t source_widget,
                                                    neui_data_item_t payload,
                                                    uint32_t allowed_actions)
  {
    return dnd_begin_drag_impl(session, source_widget, payload,
                                allowed_actions, asset_none, -1, -1);
  }

  static neui_dnd_action_t NEUI_ABI dnd_begin_drag_with_preview(
                                                    neui_session_t session,
                                                    neui_widget_t source_widget,
                                                    neui_data_item_t payload,
                                                    uint32_t allowed_actions,
                                                    const neui_drag_preview_t* preview)
  {
    if (!preview) {
      return dnd_begin_drag_impl(session, source_widget, payload,
                                  allowed_actions, asset_none, -1, -1);
    }
    return dnd_begin_drag_impl(session, source_widget, payload,
                                allowed_actions, preview->image,
                                preview->hot_x, preview->hot_y);
  }

  neui_dnd_api_t dnd_api = {
    NEUI_VERSION,
    dnd_set_drop_target,
    dnd_get_drop_target,
    dnd_set_accepted_formats,
    dnd_accept,
    dnd_begin_drag,
    dnd_begin_drag_with_preview,
  };

  // -------------------------------------------------------------------------
  // DnD platform glue: the dispatch seam is built by the shared
  // make_dnd_dispatch_seam (hosts/shared/win32/dnd_target_win32.h), whose
  // callbacks forward into the native win32 Session's dispatch_dnd_*.

  // Register the frame HWND as a drop target. Called from widget_show
  // for the root APPWINDOW / DIALOG. Idempotent across show/hide cycles.
  void register_frame_as_drop_target_w32(HWND hwnd, Session* s,
                                           uint32_t widget_id)
  {
    if (!hwnd || !s) return;
    neui_detail::dnd_ensure_ole_initialized();
    neui_detail::DndDispatchSeam seam =
      neui_detail::make_dnd_dispatch_seam(s, widget_id);
    auto* target = new neui_detail::DropTargetImpl(hwnd, seam);
    if (FAILED(RegisterDragDrop(hwnd, target))) {
      target->Release();
      return;
    }
    target->Release();
  }
}
