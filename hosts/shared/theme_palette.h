#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Host-agnostic theme palette.
//
// Both host static libs (win32 + crossplatform) read from a single
// process-wide Palette snapshot and subscribe to change notifications via
// register_theme_listener. The platform provider (theme_provider_win32.h on
// Windows; future theme_provider_macos.mm on macOS) is responsible for
// populating the palette and broadcasting on system theme changes.
//
// Header-only (function-local statics + inline) so both static libs can
// include this without ODR violations. Kept entirely portable here - no
// platform headers - so the same enum / struct can later back NSAppearance
// on macOS.

namespace neui_detail
{
  enum class ColorRole : uint16_t {
    // backgrounds
    frame_bg,                // window-level clear
    panel_bg,                // bare LABEL/BUTTON area, painted-widget clear
    control_bg,              // INPUTBOX, MULTILINE, LISTBOX, TREEVIEW field
    control_bg_alt,          // COMBOBOX dropdown overlay
    control_bg_inactive,     // selected row when widget unfocused

    // text
    text_primary,            // default widget text
    text_secondary,          // chevrons, drop-arrow glyph, dim labels
    text_disabled,           // disabled item / readonly

    // accent
    accent,                  // selection bg, slider fill, focused row
    accent_text,             // text painted on accent
    accent_translucent,      // alpha-blended selection in MULTILINE / INPUTBOX

    // borders & focus
    border,                  // unfocused outline
    border_focused,          // focused outline
    focus_ring,              // dotted/secondary focus halo

    // scrollbars
    scrollbar_track,
    scrollbar_thumb,
    scrollbar_separator,

    // IME composition (preserve current visual)
    ime_underline,
    ime_underline_target,
    ime_underline_converted,
    ime_underline_error,

    count_
  };

  struct Palette {
    uint32_t version;        // bumped on every change
    bool     is_dark;        // true if Background luminance is low
    uint32_t colors[(size_t)ColorRole::count_]; // 0xAARRGGBB
  };

  // Default palettes. The platform provider overlays these with system
  // values; any role the platform can't supply falls back to the default
  // for the active mode.
  inline const Palette& default_dark_palette()
  {
    static Palette p = []() {
      Palette pp{};
      pp.version = 1;
      pp.is_dark = true;
      pp.colors[(size_t)ColorRole::frame_bg]                = 0xFF202020;  // Win11 dark window base
      pp.colors[(size_t)ColorRole::panel_bg]                = 0xFF2C2C2C;  // slightly lifted from frame
      pp.colors[(size_t)ColorRole::control_bg]              = 0xFF1E1E1E;  // text-field surface (deeper)
      pp.colors[(size_t)ColorRole::control_bg_alt]          = 0xFF2A2A2A;
      pp.colors[(size_t)ColorRole::control_bg_inactive]     = 0xFF404040;
      pp.colors[(size_t)ColorRole::text_primary]            = 0xFFFFFFFF;
      pp.colors[(size_t)ColorRole::text_secondary]          = 0xFFCCCCCC;
      pp.colors[(size_t)ColorRole::text_disabled]           = 0xFF808080;
      pp.colors[(size_t)ColorRole::accent]                  = 0xFF3399FF;
      pp.colors[(size_t)ColorRole::accent_text]             = 0xFFFFFFFF;
      pp.colors[(size_t)ColorRole::accent_translucent]      = 0x803399FF;
      pp.colors[(size_t)ColorRole::border]                  = 0xFF888888;
      pp.colors[(size_t)ColorRole::border_focused]          = 0xFFFFFFFF;
      pp.colors[(size_t)ColorRole::focus_ring]              = 0xFFC0C0C0;
      pp.colors[(size_t)ColorRole::scrollbar_track]         = 0xFF2A2A2A;
      pp.colors[(size_t)ColorRole::scrollbar_thumb]         = 0xFF888888;
      pp.colors[(size_t)ColorRole::scrollbar_separator]     = 0xFF555555;
      pp.colors[(size_t)ColorRole::ime_underline]           = 0xFFFFFFFF;
      pp.colors[(size_t)ColorRole::ime_underline_target]    = 0xFFFFD040;
      pp.colors[(size_t)ColorRole::ime_underline_converted] = 0xFFA0A0A0;
      pp.colors[(size_t)ColorRole::ime_underline_error]     = 0xFFFF4040;
      return pp;
    }();
    return p;
  }

  inline const Palette& default_light_palette()
  {
    static Palette p = []() {
      Palette pp{};
      pp.version = 1;
      pp.is_dark = false;
      pp.colors[(size_t)ColorRole::frame_bg]                = 0xFFF3F3F3;  // Win11 light app background
      pp.colors[(size_t)ColorRole::panel_bg]                = 0xFFF3F3F3;
      pp.colors[(size_t)ColorRole::control_bg]              = 0xFFFFFFFF;
      pp.colors[(size_t)ColorRole::control_bg_alt]          = 0xFFF5F5F5;
      pp.colors[(size_t)ColorRole::control_bg_inactive]     = 0xFFE0E0E0;
      pp.colors[(size_t)ColorRole::text_primary]            = 0xFF000000;
      pp.colors[(size_t)ColorRole::text_secondary]          = 0xFF606060;
      pp.colors[(size_t)ColorRole::text_disabled]           = 0xFF8E8E8E;
      pp.colors[(size_t)ColorRole::accent]                  = 0xFF0078D4;
      pp.colors[(size_t)ColorRole::accent_text]             = 0xFFFFFFFF;
      pp.colors[(size_t)ColorRole::accent_translucent]      = 0x800078D4;
      pp.colors[(size_t)ColorRole::border]                  = 0xFFA0A0A0;
      pp.colors[(size_t)ColorRole::border_focused]          = 0xFF000000;
      pp.colors[(size_t)ColorRole::focus_ring]              = 0xFF606060;
      pp.colors[(size_t)ColorRole::scrollbar_track]         = 0xFFF0F0F0;
      pp.colors[(size_t)ColorRole::scrollbar_thumb]         = 0xFFC0C0C0;
      pp.colors[(size_t)ColorRole::scrollbar_separator]     = 0xFFD0D0D0;
      pp.colors[(size_t)ColorRole::ime_underline]           = 0xFF000000;
      pp.colors[(size_t)ColorRole::ime_underline_target]    = 0xFFFFB000;
      pp.colors[(size_t)ColorRole::ime_underline_converted] = 0xFF606060;
      pp.colors[(size_t)ColorRole::ime_underline_error]     = 0xFFD00000;
      return pp;
    }();
    return p;
  }

  // Process-wide palette singleton. Mutated only by the platform provider
  // (single UI thread); read by every paint call. Concurrent access is
  // confined to one thread so no atomics are required.
  inline Palette& mutable_current_palette()
  {
    // Default to dark to preserve today's xpl-host look until the platform
    // provider overlays it with system values.
    static Palette p = default_dark_palette();
    return p;
  }

  // Optional per-session override that wins over the system palette.
  // Sessions point this at their own effective_palette so paint code
  // (which reads via current_palette()) automatically uses the session's
  // forced LIGHT / DARK / AUTO interpretation without every paint site
  // needing a Session pointer. Single-threaded UI -> no TLS needed.
  // Multi-session apps share the override (last-set wins); typical
  // single-session use is unaffected.
  inline const Palette*& active_palette_override_ptr()
  {
    static const Palette* p = nullptr;
    return p;
  }

  inline void set_active_palette_override(const Palette* p)
  {
    active_palette_override_ptr() = p;
  }

  inline const Palette& current_palette()
  {
    if (auto* ovr = active_palette_override_ptr()) return *ovr;
    return mutable_current_palette();
  }

  // RAII helper: scope a palette override so any current_palette() read
  // inside the scope reflects `p`, and the override restores to its
  // previous value when the guard goes out of scope. Critical for
  // multi-session correctness: every entry point that reads
  // current_palette() while attached to a specific Session should wrap
  // its work in one of these (paint_frame is the canonical example;
  // on_theme_changed and platform frame-creation are the others). Without
  // it, two sessions' permanent overrides would race "last-set-wins".
  class ScopedPaletteOverride {
    const Palette* prev_;
  public:
    explicit ScopedPaletteOverride(const Palette* p)
      : prev_(active_palette_override_ptr())
    {
      set_active_palette_override(p);
    }
    ~ScopedPaletteOverride() { set_active_palette_override(prev_); }
    ScopedPaletteOverride(const ScopedPaletteOverride&) = delete;
    ScopedPaletteOverride& operator=(const ScopedPaletteOverride&) = delete;
  };

  inline uint32_t color(const Palette& p, ColorRole r)
  {
    return p.colors[(size_t)r];
  }

  inline uint32_t color(ColorRole r)
  {
    return color(current_palette(), r);
  }

  // ---- Listener registry --------------------------------------------------

  using ThemeListenerCallback = void (*)(void* token);

  struct ThemeListenerEntry {
    ThemeListenerCallback cb;
    void*                 token;
    uint32_t              handle;
  };

  inline std::vector<ThemeListenerEntry>& theme_listener_entries()
  {
    static std::vector<ThemeListenerEntry> entries;
    return entries;
  }

  inline uint32_t& theme_listener_next_handle()
  {
    static uint32_t next = 1;
    return next;
  }

  inline uint32_t register_theme_listener(ThemeListenerCallback cb, void* token)
  {
    if (!cb) return 0;
    uint32_t h = theme_listener_next_handle()++;
    theme_listener_entries().push_back({ cb, token, h });
    return h;
  }

  inline void unregister_theme_listener(uint32_t handle)
  {
    if (handle == 0) return;
    auto& v = theme_listener_entries();
    for (size_t i = 0; i < v.size(); ++i) {
      if (v[i].handle == handle) {
        v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
        return;
      }
    }
  }

  // Called by the platform provider after it repopulates the palette.
  // Snapshots the entry vector so a callback that registers / unregisters
  // mid-iteration doesn't corrupt the walk.
  inline void broadcast_theme_change()
  {
    auto snapshot = theme_listener_entries();
    for (auto& e : snapshot) {
      if (e.cb) e.cb(e.token);
    }
  }

  // ---- Helpers used by both the provider and ad-hoc widget code ----------

  // Shade an ARGB color towards black (delta < 0) or white (delta > 0).
  // Alpha is preserved. Saturates per channel.
  inline uint32_t shade(uint32_t argb, int delta)
  {
    auto clamp = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
    uint8_t a = static_cast<uint8_t>((argb >> 24) & 0xFF);
    int     r = (argb >> 16) & 0xFF;
    int     g = (argb >>  8) & 0xFF;
    int     b = (argb      ) & 0xFF;
    r = clamp(r + delta);
    g = clamp(g + delta);
    b = clamp(b + delta);
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
  }

  // Replace the alpha channel of an ARGB color, leaving RGB intact.
  inline uint32_t with_alpha(uint32_t argb, uint8_t a)
  {
    return (argb & 0x00FFFFFF) | ((uint32_t)a << 24);
  }

  // Approximate luminance (Rec. 601). Used to decide is_dark from the
  // system Background colour.
  inline uint32_t luminance_argb(uint32_t argb)
  {
    uint32_t r = (argb >> 16) & 0xFF;
    uint32_t g = (argb >>  8) & 0xFF;
    uint32_t b = (argb      ) & 0xFF;
    return (299u * r + 587u * g + 114u * b) / 1000u;
  }

} // namespace neui_detail
