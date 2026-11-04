#pragma once

#include <cstring>

// Mouse-cursor shapes, and the name <-> kind mapping behind the public
// NEUI_ATTR_CURSOR string attribute (<neui/d/attrs.h>).
//
// The enum is at GLOBAL scope on purpose. It replaces two hand-kept copies of
// the same two-value list - xpl_host::CursorKind in
// hosts/crossplatform/platform.h and an unnamed-namespace mirror in
// hosts/win32/widgets.cpp whose comment read "Mirrors xpl_host::CursorKind
// values" - so every host now shares one definition and unqualified
// NEUI_CURSOR_* keeps resolving at the existing call sites.
//
// Naming follows CSS `cursor` where CSS has an equivalent, and the parser
// accepts the CSS aliases too ("pointer" for HAND, "text" for IBEAM,
// "col-resize" for EW_RESIZE, ...). A client porting a web or JUCE UI reaches
// for those names first, and accepting them costs one strcmp.
//
// Not every shape exists natively on every OS; a platform maps what it has and
// falls back to the nearest neighbour rather than to the arrow. The
// per-platform fallbacks are documented at each platform_set_cursor
// implementation, and the notable ones (macOS has no public diagonal-resize or
// wait cursor; win32 has no open/closed hand) are listed in
// docs/deferred-issues.md.

enum neui_cursor_kind
{
  // Inherit: resolve up the widget tree, and at the root use the OS arrow.
  // This is the default for every widget, so a client sets NEUI_ATTR_CURSOR
  // only where it wants a change, and clearing it restores inheritance.
  NEUI_CURSOR_DEFAULT      = 0,

  NEUI_CURSOR_ARROW        = 1,   // explicit arrow: stop inheriting
  NEUI_CURSOR_IBEAM        = 2,   // text insertion
  NEUI_CURSOR_CROSSHAIR    = 3,
  NEUI_CURSOR_HAND         = 4,   // pointing hand (a link / clickable)
  NEUI_CURSOR_OPEN_HAND    = 5,   // draggable, not yet grabbed
  NEUI_CURSOR_CLOSED_HAND  = 6,   // grabbed, dragging
  NEUI_CURSOR_EW_RESIZE    = 7,   // horizontal double arrow (column divider)
  NEUI_CURSOR_NS_RESIZE    = 8,   // vertical double arrow (row divider)
  NEUI_CURSOR_NESW_RESIZE  = 9,   // diagonal, bottom-left / top-right
  NEUI_CURSOR_NWSE_RESIZE  = 10,  // diagonal, top-left / bottom-right
  NEUI_CURSOR_MOVE         = 11,  // four-way move / size-all
  NEUI_CURSOR_WAIT         = 12,  // busy, input blocked
  NEUI_CURSOR_PROGRESS     = 13,  // busy, input still accepted
  NEUI_CURSOR_HELP         = 14,
  NEUI_CURSOR_NOT_ALLOWED  = 15,  // rejected drop / disabled target
  NEUI_CURSOR_NONE         = 16,  // hidden (relative-pointer drags, video)

  NEUI_CURSOR_KIND_COUNT   = 17
};

namespace neui_detail
{

  // Canonical name for a kind - what get_string reads back, and the spelling
  // docs/attributes.md lists first. Never null; an out-of-range kind reads
  // back as "default" rather than crashing a client that stored a stale int.
  inline const char* cursor_kind_name(int kind)
  {
    switch (kind) {
      case NEUI_CURSOR_ARROW:       return "arrow";
      case NEUI_CURSOR_IBEAM:       return "ibeam";
      case NEUI_CURSOR_CROSSHAIR:   return "crosshair";
      case NEUI_CURSOR_HAND:        return "hand";
      case NEUI_CURSOR_OPEN_HAND:   return "open-hand";
      case NEUI_CURSOR_CLOSED_HAND: return "closed-hand";
      case NEUI_CURSOR_EW_RESIZE:   return "ew-resize";
      case NEUI_CURSOR_NS_RESIZE:   return "ns-resize";
      case NEUI_CURSOR_NESW_RESIZE: return "nesw-resize";
      case NEUI_CURSOR_NWSE_RESIZE: return "nwse-resize";
      case NEUI_CURSOR_MOVE:        return "move";
      case NEUI_CURSOR_WAIT:        return "wait";
      case NEUI_CURSOR_PROGRESS:    return "progress";
      case NEUI_CURSOR_HELP:        return "help";
      case NEUI_CURSOR_NOT_ALLOWED: return "not-allowed";
      case NEUI_CURSOR_NONE:        return "none";
      case NEUI_CURSOR_DEFAULT:
      default:                      return "default";
    }
  }

  // Parse a NEUI_ATTR_CURSOR value. Unknown / null / empty -> DEFAULT
  // (inherit), which is the safe answer: a typo leaves the cursor alone
  // instead of pinning it to an arrow and masking the mistake.
  //
  // Accepts the canonical names above plus CSS / platform aliases. Matching is
  // case-sensitive and hyphenated, matching every other string attr in the
  // framework (NEUI_ATTR_SCROLL_MODE, NEUI_ATTR_KNOB_MODE), except that '_' is
  // accepted wherever '-' is so "ew_resize" works for a C client that would
  // rather not type hyphens.
  inline int cursor_kind_from_name(const char* name)
  {
    if (!name || !name[0]) return NEUI_CURSOR_DEFAULT;

    // Normalise '_' to '-' into a small stack buffer. Longest canonical name
    // is "not-allowed" (11) / "closed-hand" (11); 24 covers every alias with
    // room to spare, and an over-long string simply won't match.
    char buf[24];
    size_t n = 0;
    for (; name[n] && n + 1 < sizeof(buf); ++n)
      buf[n] = (name[n] == '_') ? '-' : name[n];
    buf[n] = '\0';
    if (name[n]) return NEUI_CURSOR_DEFAULT;   // longer than any known name

    const char* s = buf;
    auto eq = [s](const char* lit) { return std::strcmp(s, lit) == 0; };

    if (eq("default") || eq("inherit"))       return NEUI_CURSOR_DEFAULT;
    if (eq("arrow"))                          return NEUI_CURSOR_ARROW;
    if (eq("ibeam") || eq("text"))            return NEUI_CURSOR_IBEAM;
    if (eq("crosshair") || eq("cross"))       return NEUI_CURSOR_CROSSHAIR;
    if (eq("hand") || eq("pointer"))          return NEUI_CURSOR_HAND;
    if (eq("open-hand") || eq("grab"))        return NEUI_CURSOR_OPEN_HAND;
    if (eq("closed-hand") || eq("grabbing"))  return NEUI_CURSOR_CLOSED_HAND;
    if (eq("ew-resize") || eq("col-resize") ||
        eq("e-resize")  || eq("w-resize"))    return NEUI_CURSOR_EW_RESIZE;
    if (eq("ns-resize") || eq("row-resize") ||
        eq("n-resize")  || eq("s-resize"))    return NEUI_CURSOR_NS_RESIZE;
    if (eq("nesw-resize") || eq("ne-resize") ||
        eq("sw-resize"))                      return NEUI_CURSOR_NESW_RESIZE;
    if (eq("nwse-resize") || eq("nw-resize") ||
        eq("se-resize"))                      return NEUI_CURSOR_NWSE_RESIZE;
    if (eq("move") || eq("all-scroll"))       return NEUI_CURSOR_MOVE;
    if (eq("wait") || eq("busy"))             return NEUI_CURSOR_WAIT;
    if (eq("progress"))                       return NEUI_CURSOR_PROGRESS;
    if (eq("help"))                           return NEUI_CURSOR_HELP;
    if (eq("not-allowed") || eq("no-drop") ||
        eq("forbidden"))                      return NEUI_CURSOR_NOT_ALLOWED;
    if (eq("none") || eq("hidden"))           return NEUI_CURSOR_NONE;

    return NEUI_CURSOR_DEFAULT;
  }

  // True for a kind a platform is expected to render as an actual hidden
  // pointer rather than a shape. Broken out because hiding is a *mode* on both
  // win32 (SetCursor(NULL)) and macOS ([NSCursor hide], a balanced counter),
  // so the platform layers need the same predicate.
  inline bool cursor_kind_is_hidden(int kind) { return kind == NEUI_CURSOR_NONE; }

} // namespace neui_detail
