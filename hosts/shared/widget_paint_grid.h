#pragma once

#include <neui/d/renderer.h>
#include <cstring>

#include "attrs.h"
#include "grid_model.h"
#include "scrollbar.h"
#include "theme_palette.h"
#include "widget_font.h"

// Shared GRID widget paint helper. Used by both the crossplatform host
// and the native hosts so the visual is identical regardless of which
// host instantiated the widget.
//
// Layout (widget rect):
//   +----------------------------------------+--+
//   | Header band (sticky vertically;        |  |
//   |   tracks horizontal scroll)            |  |
//   +----------------------------------------+--+
//   | Cell body                              |  | <- vertical scrollbar
//   | (clipped + scrolled by                 |  |    (visible when content
//   |  scroll_offset_x / scroll_offset_y)    |  |     exceeds viewport)
//   |                                        |  |
//   +----------------------------------------+--+
//   | horizontal scrollbar                   |[]|  <- corner square
//   +----------------------------------------+--+
//
// All coordinates are logical pixels at 96 DPI. Colours are 0xAARRGGBB.

namespace neui_detail
{
  inline constexpr float GRID_DEFAULT_FONT_SIZE   = 12.0f;
  inline constexpr float GRID_HEADER_FONT_SIZE    = 12.0f;
  inline constexpr int   GRID_FOCUS_ROW_ALPHA     = 0x55;

  // GridPaintConfig + grid_read_config live in grid_model.h so dispatch
  // code can read them without dragging the paint headers in.

  // Effective fill colour for the body area.
  inline uint32_t grid_resolve_body_bg(const GridPaintConfig& cfg)
  {
    if (cfg.bg_explicit) return cfg.bg_argb;
    return color(ColorRole::control_bg);
  }

  // Effective fill colour for the header band.
  inline uint32_t grid_resolve_header_bg(const GridPaintConfig& cfg)
  {
    (void)cfg;
    return color(ColorRole::panel_bg);
  }

  // Effective focus-row highlight colour.
  inline uint32_t grid_resolve_focus_row_color(const GridPaintConfig& cfg)
  {
    if (cfg.focus_row_color != 0) return cfg.focus_row_color;
    return with_alpha(color(ColorRole::accent), GRID_FOCUS_ROW_ALPHA);
  }

  // Main paint entry. (fx, fy) is the widget's top-left in the
  // renderer's coordinate space (host-relative on Win32 native, since
  // each painted control has its own ctx; frame-relative + translated
  // by the parent on xpl - paint_widgets_recursive applies the parent
  // transform before calling this).
  //
  // is_focused is whether the widget itself has logical focus (drives
  // the focused-vs-unfocused border + selected-row tint).
  inline void paint_grid(neui_render_backend_t* backend,
                          neui_render_ctx_t      ctx,
                          float                  fx,
                          float                  fy,
                          float                  fw,
                          float                  fh,
                          const GridModel&       m,
                          const AttrBag*         bag,
                          bool                   is_focused)
  {
    if (!backend || !ctx) return;
    if (fw <= 0 || fh <= 0) return;

    GridPaintConfig cfg = grid_read_config(bag);
    EffectiveFont   ef  = read_widget_font(bag, GRID_DEFAULT_FONT_SIZE);
    push_widget_font(backend, ctx, ef);

    GridViewport vp = grid_compute_viewport(m, (int)fw, (int)fh,
                                              cfg.row_h, cfg.header_h);

    const uint32_t body_bg     = grid_resolve_body_bg(cfg);
    const uint32_t header_bg   = grid_resolve_header_bg(cfg);
    const uint32_t text_color  = color(ColorRole::text_primary);
    const uint32_t text_dim    = color(ColorRole::text_disabled);
    const uint32_t border      = color(ColorRole::border);
    const uint32_t accent      = color(ColorRole::accent);
    const uint32_t accent_text = color(ColorRole::accent_text);
    const uint32_t sb_track    = color(ColorRole::scrollbar_track);
    const uint32_t sb_thumb    = color(ColorRole::scrollbar_thumb);
    const uint32_t sb_sep      = color(ColorRole::scrollbar_separator);
    const uint32_t focus_band  = grid_resolve_focus_row_color(cfg);

    // --- Body fill (entire widget rect first so any gap behind the
    //     scrollbars / header is filled too). The header band repaints
    //     its colour on top below.
    if (backend->fill_rect)
      backend->fill_rect(ctx, fx, fy, fw, fh, body_bg);

    // --- Body content -----------------------------------------------------
    if (vp.body_w > 0 && vp.body_h > 0 && backend->push_clip) {
      backend->push_clip(ctx, fx + (float)vp.body_x, fy + (float)vp.body_y,
                          (float)vp.body_w, (float)vp.body_h);

      int vis_rows = grid_visible_rows(vp, cfg.row_h);
      int first    = m.scroll_offset_y;
      int last     = first + vis_rows + 1;   // +1 to draw the partially-visible bottom row
      if (last > (int)m.rows.size()) last = (int)m.rows.size();

      // --- Focus-row highlight (under the cell text) ---
      if (cfg.show_focus_row && m.selected_row >= first && m.selected_row < last) {
        float ry = fy + (float)vp.body_y +
                    (float)((m.selected_row - first) * cfg.row_h);
        backend->fill_rect(ctx, fx + (float)vp.body_x, ry,
                            (float)vp.body_w, (float)cfg.row_h,
                            focus_band);
      }

      // --- Cell text per visible row ---
      int n_cols = (int)m.columns.size();
      for (int row = first; row < last; ++row) {
        const auto& rd = m.rows[(size_t)row];
        float ry = fy + (float)vp.body_y +
                    (float)((row - first) * cfg.row_h);
        float cx_running = fx + (float)vp.body_x - (float)m.scroll_offset_x;
        for (int col = 0; col < n_cols; ++col) {
          float cw = (float)m.columns[(size_t)col].width;
          if (cx_running + cw < fx + (float)vp.body_x) {
            cx_running += cw;
            continue;
          }
          if (cx_running > fx + (float)vp.body_x + (float)vp.body_w) break;

          const GridCellOverride* ov = grid_find_override(m, row, col);
          uint32_t color_for_text = text_color;
          bool     dim_cell       = false;
          if (ov) {
            if (ov->has_color)   color_for_text = ov->color;
            if (ov->has_enabled && !ov->enabled) dim_cell = true;
          }

          const std::string& cell_text = (col < (int)rd.cells.size())
                                            ? rd.cells[(size_t)col]
                                            : std::string();
          if (!cell_text.empty() && backend->draw_text) {
            if (dim_cell && backend->push_alpha) backend->push_alpha(ctx, 0.5f);
            // Horizontal alignment per column.
            float tx = cx_running + (float)GRID_CELL_PAD_X;
            float tw = cw - 2.0f * (float)GRID_CELL_PAD_X;
            if (tw < 0.0f) tw = 0.0f;
            (void)tx; (void)tw;
            // Note: the renderer's draw_text takes a rect + lets the
            // backend handle horizontal centring through its paragraph
            // alignment. For column align right/center we shift the
            // rect's left edge so the text packs into the requested
            // half - the backend still wraps inside the rect.
            switch (m.columns[(size_t)col].align) {
              case GridColAlign::Left:
                backend->draw_text(ctx, cx_running + (float)GRID_CELL_PAD_X, ry,
                                    cw - 2.0f * (float)GRID_CELL_PAD_X,
                                    (float)cfg.row_h, cell_text.c_str(),
                                    ef.size, color_for_text);
                break;
              case GridColAlign::Center: {
                if (backend->measure_text) {
                  float mw = backend->measure_text(ctx, cell_text.c_str(), -1, ef.size);
                  float inset_w = cw - 2.0f * (float)GRID_CELL_PAD_X;
                  if (inset_w < 0.0f) inset_w = 0.0f;
                  float shift = (inset_w - mw) * 0.5f;
                  if (shift < 0.0f) shift = 0.0f;
                  backend->draw_text(ctx,
                                      cx_running + (float)GRID_CELL_PAD_X + shift,
                                      ry, inset_w - shift, (float)cfg.row_h,
                                      cell_text.c_str(), ef.size, color_for_text);
                } else {
                  backend->draw_text(ctx, cx_running + (float)GRID_CELL_PAD_X, ry,
                                      cw - 2.0f * (float)GRID_CELL_PAD_X,
                                      (float)cfg.row_h, cell_text.c_str(),
                                      ef.size, color_for_text);
                }
                break;
              }
              case GridColAlign::Right: {
                if (backend->measure_text) {
                  float mw = backend->measure_text(ctx, cell_text.c_str(), -1, ef.size);
                  float inset_w = cw - 2.0f * (float)GRID_CELL_PAD_X;
                  if (inset_w < 0.0f) inset_w = 0.0f;
                  float shift = inset_w - mw;
                  if (shift < 0.0f) shift = 0.0f;
                  backend->draw_text(ctx,
                                      cx_running + (float)GRID_CELL_PAD_X + shift,
                                      ry, inset_w - shift, (float)cfg.row_h,
                                      cell_text.c_str(), ef.size, color_for_text);
                } else {
                  backend->draw_text(ctx, cx_running + (float)GRID_CELL_PAD_X, ry,
                                      cw - 2.0f * (float)GRID_CELL_PAD_X,
                                      (float)cfg.row_h, cell_text.c_str(),
                                      ef.size, color_for_text);
                }
                break;
              }
            }
            if (dim_cell && backend->pop_alpha) backend->pop_alpha(ctx);
          }
          cx_running += cw;
        }
      }

      // --- Cell-focus outline (cell_focus mode only) -------------------
      if (cfg.cell_focus && is_focused &&
          m.selected_row >= 0 && m.selected_col >= 0 &&
          m.selected_col < n_cols) {
        if (m.selected_row >= first && m.selected_row < last) {
          float ry = fy + (float)vp.body_y +
                      (float)((m.selected_row - first) * cfg.row_h);
          int col_left_content = grid_column_left(m, m.selected_col);
          int col_w            = m.columns[(size_t)m.selected_col].width;
          float cx_scr = fx + (float)vp.body_x +
                          (float)(col_left_content - m.scroll_offset_x);
          if (backend->draw_rect)
            backend->draw_rect(ctx, cx_scr, ry, (float)col_w,
                                (float)cfg.row_h, 1.0f, accent);
        }
      }

      backend->pop_clip(ctx);
    }

    // --- Header band ------------------------------------------------------
    if (vp.header_h > 0) {
      float hx = fx;
      float hy = fy;
      float hw = (float)vp.body_w;
      float hh = (float)vp.header_h;
      if (backend->fill_rect)
        backend->fill_rect(ctx, hx, hy, hw, hh, header_bg);

      if (backend->push_clip && hw > 0.0f) {
        backend->push_clip(ctx, hx, hy, hw, hh);
        int n_cols = (int)m.columns.size();
        float cx_running = hx - (float)m.scroll_offset_x;
        for (int col = 0; col < n_cols; ++col) {
          float cw = (float)m.columns[(size_t)col].width;
          if (cx_running + cw < hx) { cx_running += cw; continue; }
          if (cx_running > hx + hw) break;
          const auto& h = m.columns[(size_t)col].header;
          if (!h.empty() && backend->draw_text) {
            backend->draw_text(ctx,
                                cx_running + (float)GRID_HEADER_PAD_X, hy,
                                cw - 2.0f * (float)GRID_HEADER_PAD_X, hh,
                                h.c_str(), ef.size, text_color);
          }
          // Column divider line on the right edge of this column.
          if (backend->fill_rect) {
            backend->fill_rect(ctx,
                                cx_running + cw - 1.0f, hy + 2.0f,
                                1.0f, hh - 4.0f,
                                sb_sep);
          }
          cx_running += cw;
        }
        // Bottom separator under the header band.
        if (backend->fill_rect)
          backend->fill_rect(ctx, hx, hy + hh - 1.0f, hw, 1.0f, border);
        backend->pop_clip(ctx);
      }
    }

    // --- Vertical scrollbar ----------------------------------------------
    if (vp.vert_sb_shown) {
      int vis_rows = grid_visible_rows(vp, cfg.row_h);
      ScrollbarGeom g = compute_scrollbar(vp.body_h, 0,
                                            (int)m.rows.size(), vis_rows,
                                            m.scroll_offset_y);
      float sx = fx + (float)(vp.body_x + vp.body_w);
      float sy = fy + (float)vp.body_y;
      // Track + separator.
      if (backend->fill_rect) {
        backend->fill_rect(ctx, sx, sy, 1.0f, (float)vp.body_h, sb_sep);
        backend->fill_rect(ctx, sx + 1.0f, sy,
                            (float)(SCROLLBAR_W - 1), (float)vp.body_h, sb_track);
      }
      if (g.visible && backend->fill_rect) {
        backend->fill_rect(ctx, sx + 2.0f, sy + (float)g.thumb_pos,
                            (float)(SCROLLBAR_W - 3),
                            (float)g.thumb_len, sb_thumb);
      }
    }

    // --- Horizontal scrollbar --------------------------------------------
    if (vp.horz_sb_shown) {
      int content_w = grid_total_content_width(m);
      ScrollbarGeom g = compute_scrollbar(vp.body_w, 0,
                                            content_w, vp.body_w,
                                            m.scroll_offset_x);
      float sy = fy + (float)(vp.body_y + vp.body_h);
      float sx = fx + (float)vp.body_x;
      if (backend->fill_rect) {
        backend->fill_rect(ctx, sx, sy, (float)vp.body_w, 1.0f, sb_sep);
        backend->fill_rect(ctx, sx, sy + 1.0f, (float)vp.body_w,
                            (float)(SCROLLBAR_W - 1), sb_track);
      }
      if (g.visible && backend->fill_rect) {
        backend->fill_rect(ctx, sx + (float)g.thumb_pos, sy + 2.0f,
                            (float)g.thumb_len,
                            (float)(SCROLLBAR_W - 3), sb_thumb);
      }
    }

    // --- Corner dead square ----------------------------------------------
    if (vp.vert_sb_shown && vp.horz_sb_shown && backend->fill_rect) {
      backend->fill_rect(ctx,
                          fx + (float)(vp.body_x + vp.body_w),
                          fy + (float)(vp.body_y + vp.body_h),
                          (float)SCROLLBAR_W,
                          (float)SCROLLBAR_W,
                          sb_sep);
    }

    // --- Outer border ----------------------------------------------------
    if (backend->draw_rect) {
      backend->draw_rect(ctx, fx, fy, fw, fh, 1.0f,
                          is_focused ? color(ColorRole::border_focused) : border);
    }

    pop_widget_font(backend, ctx, ef);
  }

} // namespace neui_detail
