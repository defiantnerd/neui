#pragma once

#if defined(__APPLE__)
#import <TargetConditionals.h>
#if TARGET_OS_IPHONE

#import <UIKit/UIKit.h>

#include "../theme_palette.h"
#include "../widget_font.h"   // neui_detail::painted_ui_scale()

// iOS theme provider. UIKit twin of theme_provider_macos.h.
//
// Populates the process-wide neui_detail::Palette from UITraitCollection
// (light / dark) + UIColor system colors. Unlike macOS, iOS does NOT deliver
// appearance changes through a global / distributed notification - each UI
// object receives -traitCollectionDidChange: when its environment flips. So
// the LIVE-update path is wired through the NEUIView / NEUIViewController in
// platform_ios.mm (they override traitCollectionDidChange: and call
// refresh_theme_palette_ios), rather than installing an observer here. This
// header owns only the palette population + a seed entry point.
//
// CONVENTION (matches theme_provider_macos.h): include from exactly one
// translation unit (hosts/crossplatform/platform_ios.mm). The functions are
// inline so repeated includes within that single TU are harmless; the static
// "initialised" flag means cross-TU duplication would double-seed (harmless,
// but avoid it).

namespace neui_detail
{
  // Convert a UIColor to 0xAARRGGBB by resolving in sRGB space. Dynamic
  // colors (systemBackgroundColor etc.) must be resolved against a concrete
  // UITraitCollection for the right light/dark variant - the caller passes
  // the trait collection it wants resolved against.
  inline uint32_t uicolor_to_argb(UIColor* c, UITraitCollection* traits)
  {
    if (!c) return 0;
    // Resolve a dynamic color into the supplied trait environment so the
    // correct light/dark variant lands in the palette (iOS 13+).
    if (traits) {
      if (@available(iOS 13.0, *)) {
        c = [c resolvedColorWithTraitCollection:traits];
      }
    }
    CGFloat r = 0, g = 0, b = 0, a = 1;
    if (![c getRed:&r green:&g blue:&b alpha:&a]) {
      // Grayscale / pattern colors don't answer getRed:; fall back to white.
      CGFloat white = 1;
      if ([c getWhite:&white alpha:&a]) { r = g = b = white; }
      else return 0;
    }
    auto byte = [](CGFloat v) -> uint32_t {
      if (v < 0) v = 0;
      if (v > 1) v = 1;
      return (uint32_t)(v * 255.0 + 0.5);
    };
    return (byte(a) << 24) | (byte(r) << 16) | (byte(g) << 8) | byte(b);
  }

  // The trait collection driving the current light/dark interpretation. iOS
  // exposes UITraitCollection.currentTraitCollection during a view's
  // -traitCollectionDidChange: / layout; outside of that it reflects the
  // most recently established UI environment.
  API_AVAILABLE(ios(13.0))
  inline UITraitCollection* current_traits_ios()
  {
    return UITraitCollection.currentTraitCollection;
  }

  // ---------------------------------------------------------------------------
  // Dynamic Type for the host-PAINTED widgets.
  //
  // painted_ui_scale() (widget_font.h) scales the painted widgets' DEFAULT font
  // + layout metrics (row / line / chip heights). The canonical painted default
  // is 12px, so the scale that makes painted text match a native control is the
  // current preferred body point-size / 12.
  //
  // We use the Dynamic-Type-aware body size, NOT the constant [UIFont
  // systemFontSize] (which is a fixed 17 regardless of the user's Larger-Text /
  // accessibility setting). At the default "Large" content-size category the
  // body style resolves to ~17pt -> scale ~1.42, preserving the previous look;
  // smaller categories shrink it, larger / accessibility categories grow it.
  // Resolving against current_traits_ios() (the live UI environment, including
  // its preferredContentSizeCategory) means a content-size change recomputes to
  // the right value when called from -traitCollectionDidChange:.
  //
  // iOS-only: desktop hosts never call this, so painted_ui_scale() stays 1.0 and
  // every non-iOS host is byte-for-byte unchanged.
  inline void recompute_painted_ui_scale_ios()
  {
    UIFont* body = nil;
    if (@available(iOS 13.0, *)) {
      // Resolve the body text style against the current trait collection so the
      // user's content-size category is honoured (preferredFontForTextStyle:
      // with no traits uses the app-wide category, which is what we want here -
      // but the trait-collection overload guarantees we track a live change).
      UITraitCollection* t = current_traits_ios();
      body = t ? [UIFont preferredFontForTextStyle:UIFontTextStyleBody
                   compatibleWithTraitCollection:t]
               : [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    } else {
      body = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    }
    CGFloat pt = body ? body.pointSize : [UIFont systemFontSize];
    if (pt <= 0) pt = [UIFont systemFontSize];
    painted_ui_scale() = (float)(pt / 12.0);
  }

  inline bool is_dark_appearance_ios()
  {
    if (@available(iOS 13.0, *)) {
      UITraitCollection* t = current_traits_ios();
      return t && t.userInterfaceStyle == UIUserInterfaceStyleDark;
    }
    // Pre-iOS-13 has no dark mode.
    return false;
  }

  inline void populate_palette_ios(Palette& p)
  {
    bool dark = is_dark_appearance_ios();
    // Start from our hand-tuned defaults so any roles UIKit doesn't expose
    // (control_bg_alt, scrollbar_*, ime_underline_*) get sensible values -
    // identical strategy to populate_palette_macos so shared paint code is
    // unchanged across the two Apple platforms.
    p = dark ? default_dark_palette() : default_light_palette();
    p.is_dark = dark;

    if (@available(iOS 13.0, *)) {
      UITraitCollection* t = current_traits_ios();

      // Accent: iOS has no global "accent color" the way macOS does
      // (NSColor.controlAccentColor). UIColor.systemBlueColor is the platform
      // default tint UIKit controls use, so it is the closest analogue and
      // keeps selection highlights consistent with native controls.
      UIColor* accent  = UIColor.systemBlueColor;
      UIColor* win_bg  = UIColor.systemBackgroundColor;
      UIColor* ctrl_bg = UIColor.secondarySystemBackgroundColor;
      UIColor* text    = UIColor.labelColor;
      UIColor* sep     = UIColor.separatorColor;

      if (uint32_t v = uicolor_to_argb(accent, t); v) {
        p.colors[(size_t)ColorRole::accent] = v;
        // 50% translucent variant for selection highlights (matches macOS).
        p.colors[(size_t)ColorRole::accent_translucent] =
          (v & 0x00FFFFFFu) | 0x80000000u;
      }
      if (uint32_t v = uicolor_to_argb(win_bg, t); v) {
        p.colors[(size_t)ColorRole::frame_bg] = v;
        p.colors[(size_t)ColorRole::panel_bg] = v;
      }
      if (uint32_t v = uicolor_to_argb(ctrl_bg, t); v)
        p.colors[(size_t)ColorRole::control_bg] = v;
      if (uint32_t v = uicolor_to_argb(text, t); v)
        p.colors[(size_t)ColorRole::text_primary] = v;
      if (uint32_t v = uicolor_to_argb(sep, t); v)
        p.colors[(size_t)ColorRole::border] = v;
    }
  }

  // Repopulate the live palette and notify every listener (Session::on_theme_changed
  // repaints NEUI_ATTR_FOLLOW_SYSTEM_THEME frames). Call from
  // -traitCollectionDidChange: on the live light/dark flip.
  inline void refresh_theme_palette_ios()
  {
    Palette next{};
    populate_palette_ios(next);
    Palette& cur = mutable_current_palette();
    // Only bump version + broadcast if the resolved appearance actually
    // changed; spurious trait-change callbacks (size class, layout direction)
    // shouldn't churn the palette or trigger needless repaints.
    bool changed = next.is_dark != cur.is_dark;
    for (size_t i = 0; i < (size_t)ColorRole::count_ && !changed; ++i)
      if (next.colors[i] != cur.colors[i]) changed = true;
    if (!changed) return;
    next.version = cur.version + 1;
    cur = next;
    broadcast_theme_change();
  }

  // Idempotent. First call seeds the palette from the current appearance.
  // Live updates are driven by traitCollectionDidChange: in platform_ios.mm,
  // not by an observer installed here (iOS has no global appearance signal).
  inline void ensure_theme_provider_ios()
  {
    static bool initialised = false;
    if (initialised) return;
    initialised = true;

    Palette next{};
    populate_palette_ios(next);
    Palette& cur = mutable_current_palette();
    next.version = cur.version + 1;
    cur = next;
    broadcast_theme_change();
  }

} // namespace neui_detail

#endif // TARGET_OS_IPHONE
#endif // __APPLE__
