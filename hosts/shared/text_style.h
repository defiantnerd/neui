#pragma once

#include <cstdint>

#include <neui/d/attrs.h>

#include "attrs.h"
#include "theme_palette.h"

// Per-widget colour overrides for the text surfaces the crossplatform host
// paints itself (INPUTBOX, MULTILINE; the text colour also LABEL and BUTTON).
//
// A client with its own look - a dark editor drawn with a registered font -
// needs these widgets to follow it rather than the OS theme. The rule is the
// one NEUI_ATTR_BACKGROUND already has, applied uniformly:
//
//   attr SET   -> that ARGB, exactly (alpha included: 0x00000000 is a real,
//                 invisible colour; a translucent value blends over what lies
//                 beneath)
//   attr UNSET -> the theme palette's role, as before the attrs existed
//
// "Set" is AttrBag::has(), not "non-zero": there is no sentinel colour to steal,
// and `attrs->remove(key)` is the way back to the theme. A key stored with the
// wrong kind (set_float on an INT key) is "set" to nothing readable and falls
// back to the theme, like every other typed read.
//
// Header-only and free of any host / backend, so the resolution is Tier-1
// tested (tests/test_text_style.cpp) even though only the xpl host calls it.

namespace neui_detail
{
  // True (and `out` filled) when the widget set `key` to an int colour. A
  // missing key, a null bag and a wrong-kind slot are all "not set".
  inline bool attr_color_get(const AttrBag* bag, const char* key, uint32_t& out)
  {
    if (!bag || !bag->has(key)) return false;
    // get_int hands back its default only when the slot is not an INT (the
    // type-strict contract). Asking with two different defaults tells a stored
    // int from a wrong-kind slot, which stays on the theme instead of
    // becoming an arbitrary colour.
    const int32_t v0 = bag->get_int(key, 0);
    const int32_t v1 = bag->get_int(key, 1);
    if (v0 != v1) return false;
    out = static_cast<uint32_t>(v0);
    return true;
  }

  // The colour for `key` if the widget set it, else `fallback`.
  inline uint32_t attr_color_or(const AttrBag* bag, const char* key,
                                uint32_t fallback)
  {
    uint32_t c;
    return attr_color_get(bag, key, c) ? c : fallback;
  }

  // The five colours a text field is painted with.
  struct TextFieldStyle
  {
    uint32_t fill;            // the field's surface
    uint32_t text;            // glyphs, caret, IME composition string
    uint32_t border;          // outline, not focused
    uint32_t border_focused;  // outline, focused
    uint32_t selection;       // fill behind selected text (focused only)
  };

  // Resolve INPUTBOX / MULTILINE colours against `pal` (the palette the frame
  // paints with - pass current_palette() on the paint path).
  inline TextFieldStyle resolve_text_field_style(const AttrBag* bag,
                                                 const Palette& pal)
  {
    TextFieldStyle s;
    s.fill           = attr_color_or(bag, NEUI_ATTR_BACKGROUND,
                                     color(pal, ColorRole::control_bg));
    s.text           = attr_color_or(bag, NEUI_ATTR_TEXT_COLOR,
                                     color(pal, ColorRole::text_primary));
    s.border         = attr_color_or(bag, NEUI_ATTR_BORDER_COLOR,
                                     color(pal, ColorRole::border));
    s.border_focused = attr_color_or(bag, NEUI_ATTR_BORDER_FOCUS_COLOR,
                                     color(pal, ColorRole::border_focused));
    s.selection      = attr_color_or(bag, NEUI_ATTR_SELECTION_COLOR,
                                     color(pal, ColorRole::accent_translucent));
    return s;
  }

  // LABEL / BUTTON: just the text colour.
  inline uint32_t resolve_text_color(const AttrBag* bag, const Palette& pal)
  {
    return attr_color_or(bag, NEUI_ATTR_TEXT_COLOR,
                         color(pal, ColorRole::text_primary));
  }

} // namespace neui_detail
