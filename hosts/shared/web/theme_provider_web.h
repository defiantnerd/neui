#pragma once

// Browser dark/light tracking for the crossplatform host's web platform layer
// (plans/wasm-host.md). Mirror of linux/theme_provider_linux.h: the platform
// reads `prefers-color-scheme` in platform_init (before any Session exists, so
// the first frame's palette already matches) and again when the media query
// changes; this writes the process palette and bumps its version. The caller
// then runs broadcast_theme_change() - from the event loop, never from inside
// a DOM listener (the broadcast reaches client callbacks).

#include "../theme_palette.h"

#include <cstdint>

namespace neui_detail
{
  // scheme: 1 = the page prefers dark, 2 = prefers light, 0 = neither. "No
  // preference" keeps neui's historical default (dark), as on Linux.
  inline void theme_apply_web_scheme(int scheme)
  {
    const bool dark = (scheme != 2);
    Palette& p = mutable_current_palette();
    const uint32_t prev_version = p.version;
    p = dark ? default_dark_palette() : default_light_palette();
    p.version = prev_version + 1;   // caches / sessions notice the change
  }
}
