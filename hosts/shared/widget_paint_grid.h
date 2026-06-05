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

  // Glyph metrics for the sort indicator in the header band. The glyph is a
  // filled triangle drawn through the path API; the multi-level digit is a
  // small text run rendered next to it. Tight fits when the column is wide
  // enough; otherwise we just clip the trailing text and trust the existing
  // header-cell clip.
  inline constexpr float GRID_SORT_GLYPH_W   = 7.0f;
  inline constexpr float GRID_SORT_GLYPH_H   = 5.0f;
  inline constexpr float GRID_SORT_GLYPH_GAP = 4.0f;   // gap between text and glyph
  inline constexpr float GRID_SORT_DIGIT_GAP = 2.0f;   // gap between glyph and digit

  // Draw the sort indicator (triangle + optional level digit) right-aligned
  // inside the header cell. `cx` is the cell's logical left edge AFTER the
  // horizontal scroll has been applied. Returns the px width consumed so the
  // caller can shrink the header text rect.
  inline float grid_paint_sort_indicator(neui_render_backend_t* backend,
                                          neui_render_ctx_t      ctx,
                                          float                  cx,
                                          float                  cy,
                                          float                  cw,
                                          float                  ch,
                                          float                  font_size,
                                          neui_grid_sort_dir_t   dir,
                                          int                    level,
                                          int                    total_levels,
                                          uint32_t               color_argb)
  {
    if (!backend || !backend->fill_path) return 0.0f;
    // Digit only when there are multiple levels (the common single-sort case
    // stays clean).
    char digit_buf[4] = { 0 };
    float digit_w = 0.0f;
    if (total_levels > 1) {
      // level is 0-based; show 1-based to the user. Clamp to 9 (single digit).
      int shown = level + 1;
      if (shown > 9) shown = 9;
      digit_buf[0] = (char)('0' + shown);
      if (backend->measure_text)
        digit_w = backend->measure_text(ctx, digit_buf, -1, font_size);
    }
    float total_w = GRID_SORT_GLYPH_W +
                     (digit_w > 0.0f ? (GRID_SORT_DIGIT_GAP + digit_w) : 0.0f);
    // Right-align inside the cell, with the same header-pad inset.
    float right = cx + cw - (float)GRID_HEADER_PAD_X;
    float gx    = right - total_w;
    // Vertically centre the glyph against the header band.
    float gy    = cy + (ch - GRID_SORT_GLYPH_H) * 0.5f;

    backend->begin_path(ctx);
    if (dir == NEUI_GRID_SORT_ASC) {
      // Triangle pointing UP (apex at top).
      backend->move_to(ctx, gx + GRID_SORT_GLYPH_W * 0.5f, gy);
      backend->line_to(ctx, gx + GRID_SORT_GLYPH_W,        gy + GRID_SORT_GLYPH_H);
      backend->line_to(ctx, gx,                            gy + GRID_SORT_GLYPH_H);
    } else {
      // Triangle pointing DOWN (apex at bottom).
      backend->move_to(ctx, gx,                            gy);
      backend->line_to(ctx, gx + GRID_SORT_GLYPH_W,        gy);
      backend->line_to(ctx, gx + GRID_SORT_GLYPH_W * 0.5f, gy + GRID_SORT_GLYPH_H);
    }
    backend->close_path(ctx);
    backend->fill_path(ctx, color_argb);

    if (digit_w > 0.0f && backend->draw_text) {
      backend->draw_text(ctx,
                          gx + GRID_SORT_GLYPH_W + GRID_SORT_DIGIT_GAP, cy,
                          digit_w, ch,
                          digit_buf, font_size, color_argb);
    }
    return total_w + GRID_SORT_GLYPH_GAP;  // consumed width including trailing gap
  }

  // Main paint entry. (fx, fy) is the widget's top-left in the
  // renderer's coordinate space (host-relative on Win32 native, since
  // each painted control has its own ctx; frame-relative + translated
  // by the parent on xpl - paint_widgets_recursive applies the parent
  // transform before calling this).
  //
  // is_focused is whether the widget itself has logical focus (drives
  // the focused-vs-unfocused border + selected-row tint).
  //
  // Takes the model by mutable reference so a sort_dirty flag (set by row
  // mutations / sort changes between paints) can rebuild display_order
  // lazily here.
  inline void paint_grid(neui_render_backend_t* backend,
                          neui_render_ctx_t      ctx,
                          float                  fx,
                          float                  fy,
                          float                  fw,
                          float                  fh,
                          GridModel&             m,
                          const AttrBag*         bag,
                          bool                   is_focused)
  {
    if (!backend || !ctx) return;
    if (fw <= 0 || fh <= 0) return;

    grid_ensure_sort_clean(m);

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
      // Fine sub-row pixel offset (macOS smooth scroll / rubber-band; 0 on
      // win32 / xpl). Content is shifted up by pxoff, so the body window
      // starts mid-row; draw one extra trailing row when pxoff != 0 to fill
      // the gap a fractional offset opens at the bottom.
      const float pxoff = (float)m.scroll_px_offset;
      int last     = first + vis_rows + (m.scroll_px_offset != 0 ? 2 : 1);
      if (last > (int)m.rows.size()) last = (int)m.rows.size();

      // --- Focus-row highlight (under the cell text) ---
      // selected_row is a LOGICAL index; translate to visual position so
      // the band paints under whatever row the user sees as selected after
      // sorting. Identity mapping when no sort is active.
      int selected_visual = grid_logical_to_visual(m, m.selected_row);
      if (cfg.show_focus_row && selected_visual >= first && selected_visual < last) {
        float ry = fy + (float)vp.body_y - pxoff +
                    (float)((selected_visual - first) * cfg.row_h);
        backend->fill_rect(ctx, fx + (float)vp.body_x, ry,
                            (float)vp.body_w, (float)cfg.row_h,
                            focus_band);
      }

      // --- Cell text per visible row ---
      // The loop walks VISUAL row indices; grid_visual_to_logical resolves
      // each to the logical row whose data we paint (identity when unsorted).
      int n_cols = (int)m.columns.size();
      for (int vrow = first; vrow < last; ++vrow) {
        int row = grid_visual_to_logical(m, vrow);
        if (row < 0) continue;
        const auto& rd = m.rows[(size_t)row];
        float ry = fy + (float)vp.body_y - pxoff +
                    (float)((vrow - first) * cfg.row_h);
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
      // selected_row is logical; translate to its visual position so the
      // outline tracks the row the user sees after sorting. Suppressed
      // while the in-place editor is open over this cell - the editor
      // draws its own (heavier) accent border below.
      bool edit_over_selected = m.edit.active &&
                                m.edit.row == m.selected_row &&
                                m.edit.col == m.selected_col;
      if (cfg.cell_focus && is_focused &&
          m.selected_row >= 0 && m.selected_col >= 0 &&
          m.selected_col < n_cols && !edit_over_selected) {
        int sel_v = selected_visual;
        if (sel_v >= first && sel_v < last) {
          float ry = fy + (float)vp.body_y - pxoff +
                      (float)((sel_v - first) * cfg.row_h);
          int col_left_content = grid_column_left(m, m.selected_col);
          int col_w            = m.columns[(size_t)m.selected_col].width;
          float cx_scr = fx + (float)vp.body_x +
                          (float)(col_left_content - m.scroll_offset_x);
          if (backend->draw_rect)
            backend->draw_rect(ctx, cx_scr, ry, (float)col_w,
                                (float)cfg.row_h, 1.0f, accent);
        }
      }

      // --- In-place cell editor overlay --------------------------------
      // Paints over the underlying cell text: solid control_bg fill, 2 px
      // accent border, optional selection rectangle, working text, caret.
      // The model stores LOGICAL (row, col); translate to visual for the
      // on-screen y.
      if (m.edit.active && m.edit.col >= 0 && m.edit.col < n_cols) {
        int edit_v = grid_logical_to_visual(m, m.edit.row);
        if (edit_v >= first && edit_v < last) {
          float ry = fy + (float)vp.body_y - pxoff +
                      (float)((edit_v - first) * cfg.row_h);
          int   col_left_content = grid_column_left(m, m.edit.col);
          float cw               = (float)m.columns[(size_t)m.edit.col].width;
          float cx_scr = fx + (float)vp.body_x +
                          (float)(col_left_content - m.scroll_offset_x);
          // Cell background (cover whatever was painted underneath - cell
          // text, focus-row band, etc.).
          if (backend->fill_rect)
            backend->fill_rect(ctx, cx_scr, ry, cw,
                                (float)cfg.row_h, body_bg);
          // 2 px accent border (drawn as two stroked rects so a 1 px
          // backend doesn't need a width param above 1).
          if (backend->draw_rect) {
            backend->draw_rect(ctx, cx_scr, ry, cw, (float)cfg.row_h,
                                1.0f, accent);
            backend->draw_rect(ctx, cx_scr + 1.0f, ry + 1.0f,
                                cw - 2.0f, (float)cfg.row_h - 2.0f,
                                1.0f, accent);
          }
          // Working text - always left-aligned regardless of column align;
          // the editor is a flat text field, the alignment was a paint-time
          // concern for the static value.
          float text_x = cx_scr + (float)GRID_CELL_PAD_X;
          float text_w = cw - 2.0f * (float)GRID_CELL_PAD_X;
          if (text_w < 0.0f) text_w = 0.0f;
          const std::string& et = m.edit.te.text;
          int  cursor = m.edit.te.cursor;
          int  anchor = m.edit.te.sel_anchor;
          if (cursor < 0) cursor = 0;
          if (cursor > (int)et.size()) cursor = (int)et.size();
          if (anchor < 0) anchor = 0;
          if (anchor > (int)et.size()) anchor = (int)et.size();

          // Selection rectangle (behind the text). Measure the prefix +
          // post-prefix to find the on-screen extents.
          if (anchor != cursor && backend->measure_text && backend->fill_rect) {
            int lo = te_sel_lo(cursor, anchor);
            int hi = te_sel_hi(cursor, anchor);
            std::string pre(et,  0,         (size_t)lo);
            std::string mid(et,  (size_t)lo, (size_t)(hi - lo));
            float pre_w = backend->measure_text(ctx, pre.c_str(), -1, ef.size);
            float mid_w = backend->measure_text(ctx, mid.c_str(), -1, ef.size);
            float sx = text_x + pre_w;
            float sy = ry + 2.0f;
            float sh = (float)cfg.row_h - 4.0f;
            // Clip the selection rect against the cell interior so a long
            // selection doesn't spill over the border.
            float right = cx_scr + cw - 2.0f;
            float sx_clamped  = (sx < cx_scr + 2.0f) ? cx_scr + 2.0f : sx;
            float swid        = (sx + mid_w > right) ? (right - sx_clamped)
                                                     : (mid_w - (sx_clamped - sx));
            if (swid > 0.0f) {
              // Same translucent accent as INPUTBOX / MULTILINE selection,
              // so the grid editor matches the rest of the framework's
              // text-selection look.
              uint32_t sel_bg = color(ColorRole::accent_translucent);
              backend->fill_rect(ctx, sx_clamped, sy, swid, sh, sel_bg);
            }
          }

          if (!et.empty() && backend->draw_text) {
            backend->draw_text(ctx, text_x, ry, text_w, (float)cfg.row_h,
                                et.c_str(), ef.size, text_color);
          }
          // Caret. Measure the prefix up to the byte cursor; the backend
          // owns the font metrics, so a measure-then-draw is the cleanest
          // way to land the caret on a codepoint boundary.
          float caret_dx = 0.0f;
          if (cursor > 0 && backend->measure_text) {
            std::string prefix(et, 0, (size_t)cursor);
            caret_dx = backend->measure_text(ctx, prefix.c_str(), -1, ef.size);
          }
          float caret_x = text_x + caret_dx;
          float right_clip = cx_scr + cw - 2.0f;
          if (caret_x > right_clip) caret_x = right_clip;
          if (caret_x < cx_scr + 2.0f) caret_x = cx_scr + 2.0f;
          if (backend->fill_rect) {
            float caret_top = ry + 3.0f;
            float caret_h   = (float)cfg.row_h - 6.0f;
            if (caret_h < 4.0f) caret_h = (float)cfg.row_h - 2.0f;
            backend->fill_rect(ctx, caret_x, caret_top, 1.0f, caret_h,
                                text_color);
          }
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
        int n_cols       = (int)m.columns.size();
        int total_levels = (int)m.sort_stack.size();
        const uint32_t glyph_primary   = color(ColorRole::accent);
        const uint32_t glyph_secondary = color(ColorRole::text_secondary);
        float cx_running = hx - (float)m.scroll_offset_x;
        for (int col = 0; col < n_cols; ++col) {
          float cw = (float)m.columns[(size_t)col].width;
          if (cx_running + cw < hx) { cx_running += cw; continue; }
          if (cx_running > hx + hw) break;

          // Sort indicator (drawn first so the header text rect can shrink
          // around it; without this a long header would draw over the glyph).
          float indicator_w = 0.0f;
          int   level       = grid_sort_stack_find(m, col);
          if (level >= 0) {
            uint32_t col_glyph = (level == 0) ? glyph_primary : glyph_secondary;
            indicator_w = grid_paint_sort_indicator(
              backend, ctx, cx_running, hy, cw, hh, ef.size,
              m.sort_stack[(size_t)level].dir,
              level, total_levels, col_glyph);
          }

          const auto& h = m.columns[(size_t)col].header;
          if (!h.empty() && backend->draw_text) {
            float text_w = cw - 2.0f * (float)GRID_HEADER_PAD_X - indicator_w;
            if (text_w < 0.0f) text_w = 0.0f;
            backend->draw_text(ctx,
                                cx_running + (float)GRID_HEADER_PAD_X, hy,
                                text_w, hh,
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
