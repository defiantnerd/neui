#pragma once

#include <cstring>
#include <cstdint>

// Shared, host-neutral geometry for the tabbed view (NEUI_W_TABVIEW).
// Pure logic - no backend, no widget tree - so the crossplatform, win32, and
// macOS hosts all lay tabs out identically, and the Tier-1 unit suite can
// cover it without a display.
//
// All coordinates are WIDGET-LOCAL logical pixels (origin 0,0 at the
// tabview's top-left). The caller offsets by the widget's screen position
// when painting / hit-testing, exactly like the SECTION scroll helpers.

namespace neui_detail
{
  // ---- Tunables -----------------------------------------------------------
  inline constexpr float TAB_STRIP_SIZE_DEFAULT = 28.0f; // band thickness
  inline constexpr float TAB_CHIP_PAD_X         = 10.0f; // text inset (horizontal strips)
  inline constexpr float TAB_CHIP_PAD_Y         = 4.0f;  // text inset (vertical strips)
  inline constexpr float TAB_CHIP_GAP           = 2.0f;  // gap between adjacent chips
  inline constexpr float TAB_CHIP_FONT          = 12.0f; // chip label size
  inline constexpr float TAB_CHIP_MIN_W         = 28.0f; // min chip extent (horizontal)
  inline constexpr float TAB_BORDER_WIDTH_DEFAULT = 1.0f;

  // Edge the chip strip sits on, and how the chips pack along that edge.
  enum class TabEdge  : uint8_t { Top, Bottom, Left, Right, None };
  enum class TabAlign : uint8_t { Begin, Center, End };

  struct TabPosition { TabEdge edge; TabAlign align; };

  // Parse NEUI_ATTR_TAB_POSITION (e.g. "top-left", "left-bottom", "none").
  // For top/bottom the alignment token is horizontal (left/center/right);
  // for left/right it is vertical (top/center/bottom). Unset / unrecognised
  // defaults to { Top, Begin } ("top-left").
  inline TabPosition parse_tab_position(const char* s)
  {
    if (!s || !*s) return { TabEdge::Top, TabAlign::Begin };
    if (!std::strcmp(s, "none")) return { TabEdge::None, TabAlign::Begin };

    TabEdge edge = TabEdge::Top;
    const char* dash = std::strchr(s, '-');
    auto edge_is = [&](const char* e) {
      size_t n = dash ? (size_t)(dash - s) : std::strlen(s);
      return std::strlen(e) == n && std::strncmp(s, e, n) == 0;
    };
    if      (edge_is("top"))    edge = TabEdge::Top;
    else if (edge_is("bottom")) edge = TabEdge::Bottom;
    else if (edge_is("left"))   edge = TabEdge::Left;
    else if (edge_is("right"))  edge = TabEdge::Right;

    TabAlign align = TabAlign::Begin;
    const char* a = dash ? dash + 1 : "";
    if      (!std::strcmp(a, "center")) align = TabAlign::Center;
    else if (!std::strcmp(a, "right"))  align = TabAlign::End;
    else if (!std::strcmp(a, "bottom")) align = TabAlign::End;
    else                                align = TabAlign::Begin; // left / top / empty
    return { edge, align };
  }

  // Strip band + content body, in widget-local coords.
  inline bool tab_edge_is_horizontal(TabEdge e);  // fwd

  // Resolve the strip thickness. An explicit NEUI_ATTR_TAB_STRIP_SIZE wins.
  // Otherwise horizontal strips use the default height; VERTICAL strips
  // (left / right) auto-fit the widest chip label so the text is fully
  // readable (text + 2*pad), floored at the default. `text_widths` are the
  // measured label widths (logical px).
  inline float tab_resolve_strip_size(TabEdge edge, float explicit_size,
                                      const float* text_widths, int count)
  {
    if (explicit_size > 0.0f) return explicit_size;
    if (edge == TabEdge::Top || edge == TabEdge::Bottom || edge == TabEdge::None)
      return TAB_STRIP_SIZE_DEFAULT;
    float widest = 0.0f;
    for (int i = 0; i < count; ++i)
      if (text_widths[i] > widest) widest = text_widths[i];
    float s = widest + 2.0f * TAB_CHIP_PAD_X;
    return (s < TAB_STRIP_SIZE_DEFAULT) ? TAB_STRIP_SIZE_DEFAULT : s;
  }

  struct TabViewLayout {
    float strip_x, strip_y, strip_w, strip_h; // chip band (zero-sized for None)
    float body_x,  body_y,  body_w,  body_h;   // content area for the active page
  };

  inline TabViewLayout compute_tabview_layout(float w, float h,
                                              TabEdge edge, float strip)
  {
    if (strip <= 0.0f) strip = TAB_STRIP_SIZE_DEFAULT;
    TabViewLayout L{};
    switch (edge) {
      case TabEdge::Top:
        if (strip > h) strip = h;
        L.strip_x = 0; L.strip_y = 0;        L.strip_w = w;     L.strip_h = strip;
        L.body_x  = 0; L.body_y  = strip;    L.body_w  = w;     L.body_h  = h - strip;
        break;
      case TabEdge::Bottom:
        if (strip > h) strip = h;
        L.strip_x = 0; L.strip_y = h - strip; L.strip_w = w;    L.strip_h = strip;
        L.body_x  = 0; L.body_y  = 0;          L.body_w = w;     L.body_h  = h - strip;
        break;
      case TabEdge::Left:
        if (strip > w) strip = w;
        L.strip_x = 0;     L.strip_y = 0; L.strip_w = strip;     L.strip_h = h;
        L.body_x  = strip; L.body_y  = 0; L.body_w  = w - strip; L.body_h  = h;
        break;
      case TabEdge::Right:
        if (strip > w) strip = w;
        L.strip_x = w - strip; L.strip_y = 0; L.strip_w = strip; L.strip_h = h;
        L.body_x  = 0;         L.body_y  = 0; L.body_w = w-strip; L.body_h  = h;
        break;
      case TabEdge::None:
      default:
        L.strip_x = L.strip_y = L.strip_w = L.strip_h = 0;
        L.body_x  = 0; L.body_y = 0; L.body_w = w; L.body_h = h;
        break;
    }
    if (L.body_w < 0) L.body_w = 0;
    if (L.body_h < 0) L.body_h = 0;
    return L;
  }

  struct TabChip {
    float x, y, w, h;     // chip rect, widget-local
    float text_x, text_w; // padded text band inside the chip
  };

  inline bool tab_edge_is_horizontal(TabEdge e) {
    return e == TabEdge::Top || e == TabEdge::Bottom;
  }

  // Lay out one chip per tab along the strip. `text_widths[i]` is the
  // measured width of tab i's label (used only for horizontal strips; on
  // vertical strips chips are fixed-height rows spanning the strip width).
  // `out` must have room for `count` entries. Returns nothing; out chips are
  // in widget-local coords. For TabEdge::None no chips are produced (the
  // caller should not call this).
  inline void layout_tab_chips(const TabViewLayout& L, TabEdge edge,
                               TabAlign align, const float* text_widths,
                               int count, TabChip* out)
  {
    if (count <= 0) return;
    const bool horiz = tab_edge_is_horizontal(edge);

    if (horiz) {
      // Per-chip widths + total extent.
      float total = 0.0f;
      for (int i = 0; i < count; ++i) {
        float cw = text_widths[i] + 2.0f * TAB_CHIP_PAD_X;
        if (cw < TAB_CHIP_MIN_W) cw = TAB_CHIP_MIN_W;
        out[i].w = cw;
        total += cw + (i ? TAB_CHIP_GAP : 0.0f);
      }
      float start = L.strip_x;
      if (align == TabAlign::Center)   start = L.strip_x + (L.strip_w - total) * 0.5f;
      else if (align == TabAlign::End) start = L.strip_x + (L.strip_w - total);
      if (start < L.strip_x) start = L.strip_x;
      float cx = start;
      for (int i = 0; i < count; ++i) {
        out[i].x = cx;
        out[i].y = L.strip_y;
        out[i].h = L.strip_h;
        out[i].text_x = cx + TAB_CHIP_PAD_X;
        out[i].text_w = out[i].w - 2.0f * TAB_CHIP_PAD_X;
        if (out[i].text_w < 0) out[i].text_w = 0;
        cx += out[i].w + TAB_CHIP_GAP;
      }
    } else {
      // Vertical strip: fixed-height rows, full strip width, horizontal text.
      float row_h = TAB_CHIP_FONT + 2.0f * TAB_CHIP_PAD_Y + 8.0f;
      float total = count * row_h + (count - 1) * TAB_CHIP_GAP;
      float start = L.strip_y;
      if (align == TabAlign::Center)   start = L.strip_y + (L.strip_h - total) * 0.5f;
      else if (align == TabAlign::End) start = L.strip_y + (L.strip_h - total);
      if (start < L.strip_y) start = L.strip_y;
      float cy = start;
      for (int i = 0; i < count; ++i) {
        out[i].x = L.strip_x;
        out[i].y = cy;
        out[i].w = L.strip_w;
        out[i].h = row_h;
        out[i].text_x = L.strip_x + TAB_CHIP_PAD_X;
        out[i].text_w = L.strip_w - 2.0f * TAB_CHIP_PAD_X;
        if (out[i].text_w < 0) out[i].text_w = 0;
        cy += row_h + TAB_CHIP_GAP;
      }
    }
  }

  // Hit-test a widget-local point against the chips. Returns the chip index
  // or -1 if none. `count` chips in `chips`.
  inline int tabview_chip_hit(const TabChip* chips, int count,
                              float lx, float ly)
  {
    for (int i = 0; i < count; ++i) {
      const TabChip& c = chips[i];
      if (lx >= c.x && lx < c.x + c.w && ly >= c.y && ly < c.y + c.h)
        return i;
    }
    return -1;
  }

} // namespace neui_detail
