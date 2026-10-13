#pragma once

// The neui-drawn message box, for platform layers with no native one (Linux /
// X11, the web). Owns the MessageBoxEx-shaped NEUI_M_* decode (button set,
// default button, Esc / cancel button, icon class, in Win32 visual order with
// the matching NEUI_ID_* returns) plus the backend-only layout, render and
// hit-test. The platform layer supplies the window, its render context and
// the modal loop, and scopes the owner session's palette around layout() and
// render() (ScopedPaletteOverride) so dark / light + accent match.

#include <neui/neui.h>
#include <neui/d/notify.h>

#include "theme_palette.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace neui_detail
{
  struct MsgBoxButton { const char* label; int id; };

  struct MsgBoxSpec
  {
    MsgBoxButton btn[3]   = {};
    int          count    = 0;
    int          def_index    = 0;   // Enter activates this button
    int          cancel_index = -1;  // Esc / close activates this button (-1 = none)
    uint32_t     icon     = 0;       // NEUI_MB_ICON* nibble (0x10/0x20/0x30/0x40) or 0
  };

  inline MsgBoxSpec msgbox_parse(uint32_t flags)
  {
    MsgBoxSpec s;
    s.icon = flags & 0x00F0u;
    auto set = [&](int i, const char* l, int id) { s.btn[i].label = l; s.btn[i].id = id; };
    switch (flags & 0x000Fu) {
      default:
      case NEUI_MB_OK:
        set(0, "OK", NEUI_ID_OK); s.count = 1; break;
      case NEUI_MB_OKCANCEL:
        set(0, "OK", NEUI_ID_OK); set(1, "Cancel", NEUI_ID_CANCEL); s.count = 2; break;
      case NEUI_MB_ABORTRETRYIGNORE:
        set(0, "Abort", NEUI_ID_ABORT); set(1, "Retry", NEUI_ID_RETRY);
        set(2, "Ignore", NEUI_ID_IGNORE); s.count = 3; break;
      case NEUI_MB_YESNOCANCEL:
        set(0, "Yes", NEUI_ID_YES); set(1, "No", NEUI_ID_NO);
        set(2, "Cancel", NEUI_ID_CANCEL); s.count = 3; break;
      case NEUI_MB_YESNO:
        set(0, "Yes", NEUI_ID_YES); set(1, "No", NEUI_ID_NO); s.count = 2; break;
      case NEUI_MB_RETRYCANCEL:
        set(0, "Retry", NEUI_ID_RETRY); set(1, "Cancel", NEUI_ID_CANCEL); s.count = 2; break;
      case NEUI_MB_CANCELTRYCONTINUE:
        set(0, "Cancel", NEUI_ID_CANCEL); set(1, "Try Again", NEUI_ID_TRYAGAIN);
        set(2, "Continue", NEUI_ID_CONTINUE); s.count = 3; break;
    }
    int def = (int)((flags & 0x0F00u) >> 8);
    if (def >= 0 && def < s.count) s.def_index = def;
    // Esc maps to Cancel if present, else to the sole OK button, else nothing.
    for (int i = 0; i < s.count; ++i)
      if (s.btn[i].id == NEUI_ID_CANCEL) s.cancel_index = i;
    if (s.cancel_index < 0 && s.count == 1) s.cancel_index = 0;
    return s;
  }

  // Layout + paint of one box, all in logical px. Usage: layout() (measures
  // through an off-screen context), size the window to win_w x win_h, then
  // render() into its context whenever hover / pressed change.
  struct MsgBoxPainted
  {
    MsgBoxSpec spec;
    std::vector<std::string> cap_lines, txt_lines;
    int win_w = 0, win_h = 0;
    int btn_y = 0;
    int bx[3] = {0, 0, 0}, bw[3] = {0, 0, 0};
    int hover = -1, pressed = -1;   // button index or -1

    static constexpr float SZ_TXT = 13.0f, SZ_CAP = 14.0f;
    static constexpr int   LH = 18, LH_CAP = 20, WRAP = 360;
    static constexpr int   PAD = 18, GAP = 12, BTN_H = 28, BTN_GAP = 8, BTN_MINW = 84, ICON = 32;

    bool layout(neui_render_backend_t* backend, const char* text, const char* caption,
                uint32_t flags)
    {
      spec = msgbox_parse(flags);
      cap_lines.clear(); txt_lines.clear();
      if (!backend) return false;

      neui_render_ctx_t mctx = backend->create_offscreen_context(8, 8, 1.0f);
      auto meas = [&](const char* s, float sz, int weight) -> int {
        if (!mctx || !s || !*s) return 0;
        if (weight) backend->push_font(mctx, "", weight);
        int w = (int)backend->measure_text(mctx, s, -1, sz);
        if (weight) backend->pop_font(mctx);
        return w;
      };

      // Word-wrap UTF-8 `s` to maxw logical px (honours embedded '\n').
      auto wrap = [&](const char* s, float sz, int weight, int maxw,
                      std::vector<std::string>& out) {
        if (!s || !*s) return;
        std::string cur, word;
        auto flush_word = [&]() {
          if (word.empty()) return;
          std::string trial = cur.empty() ? word : cur + " " + word;
          if (cur.empty() || meas(trial.c_str(), sz, weight) <= maxw) cur = trial;
          else { out.push_back(cur); cur = word; }
          word.clear();
        };
        for (const char* p = s;; ++p) {
          char c = *p;
          if (c == '\n' || c == 0) { flush_word(); out.push_back(cur); cur.clear();
                                     if (c == 0) break; }
          else if (c == ' ') flush_word();
          else word += c;
        }
      };

      wrap(caption, SZ_CAP, 700, WRAP, cap_lines);
      wrap(text,    SZ_TXT, 0,   WRAP, txt_lines);

      int text_w = 0;
      for (auto& l : cap_lines) text_w = std::max(text_w, meas(l.c_str(), SZ_CAP, 700));
      for (auto& l : txt_lines) text_w = std::max(text_w, meas(l.c_str(), SZ_TXT, 0));
      int content_h = (int)cap_lines.size() * LH_CAP + (int)txt_lines.size() * LH;
      if (!cap_lines.empty() && !txt_lines.empty()) content_h += 6;

      int total_b = 0;
      for (int i = 0; i < spec.count; ++i) {
        bw[i] = std::max(BTN_MINW, meas(spec.btn[i].label, SZ_TXT, 0) + 24);
        total_b += bw[i] + (i ? BTN_GAP : 0);
      }
      int icon_w = spec.icon ? ICON + GAP : 0;
      int left_h = std::max(content_h, spec.icon ? ICON : 0);
      win_w = PAD * 2 + std::max(icon_w + text_w, total_b);
      win_h = PAD * 2 + left_h + GAP + BTN_H;
      if (mctx) backend->destroy_context(mctx);

      btn_y = win_h - PAD - BTN_H;
      for (int i = 0, cx = win_w - PAD - total_b; i < spec.count; ++i) {
        bx[i] = cx;
        cx += bw[i] + BTN_GAP;
      }
      return true;
    }

    int hit_button(int lx, int ly) const
    {
      if (ly < btn_y || ly >= btn_y + BTN_H) return -1;
      for (int i = 0; i < spec.count; ++i)
        if (lx >= bx[i] && lx < bx[i] + bw[i]) return i;
      return -1;
    }

    // The result for Enter, and for Esc / closing the window (0 when the box
    // has no cancel button: Esc does nothing, closing returns 0).
    int default_result() const { return spec.btn[spec.def_index].id; }
    int cancel_result() const
    {
      return spec.cancel_index >= 0 ? spec.btn[spec.cancel_index].id : 0;
    }

    // One whole frame: begin_frame .. end_frame.
    void render(neui_render_backend_t* backend, neui_render_ctx_t ctx) const
    {
      auto C = [](ColorRole r) { return current_palette().colors[(size_t)r]; };
      backend->begin_frame(ctx, C(ColorRole::frame_bg));
      if (spec.icon) {
        uint32_t icol; const char* glyph;
        switch (spec.icon) {
          case NEUI_MB_ICONERROR:       icol = 0xFFD13438; glyph = "\xC3\x97"; break; // ×
          case NEUI_MB_ICONWARNING:     icol = 0xFFF7630C; glyph = "!";        break;
          case NEUI_MB_ICONQUESTION:    icol = 0xFF0078D4; glyph = "?";        break;
          default: /* INFORMATION */    icol = 0xFF0078D4; glyph = "i";        break;
        }
        int cx = PAD + ICON / 2, cy = PAD + ICON / 2, rr = ICON / 2;
        backend->begin_path(ctx);
        backend->arc(ctx, (float)cx, (float)cy, (float)rr, 0.0f, 6.2831853f);
        backend->close_path(ctx);
        backend->fill_path(ctx, icol);
        backend->push_font(ctx, "", 700);
        int gw = (int)backend->measure_text(ctx, glyph, -1, 18.0f);
        backend->draw_text(ctx, (float)(cx - gw / 2), (float)PAD, (float)(gw + 4), (float)ICON, glyph, 18.0f, 0xFFFFFFFF);
        backend->pop_font(ctx);
      }
      int tx = PAD + (spec.icon ? ICON + GAP : 0);
      int ty = PAD;
      backend->push_font(ctx, "", 700);
      for (auto& l : cap_lines) {
        backend->draw_text(ctx, (float)tx, (float)ty, (float)(win_w - tx - PAD), (float)LH_CAP, l.c_str(), SZ_CAP,
                           C(ColorRole::text_primary));
        ty += LH_CAP;
      }
      backend->pop_font(ctx);
      if (!cap_lines.empty() && !txt_lines.empty()) ty += 6;
      for (auto& l : txt_lines) {
        backend->draw_text(ctx, (float)tx, (float)ty, (float)(win_w - tx - PAD), (float)LH, l.c_str(), SZ_TXT,
                           C(ColorRole::text_primary));
        ty += LH;
      }
      for (int i = 0; i < spec.count; ++i) {
        uint32_t fill = C(ColorRole::control_bg);
        if (pressed == i)     fill = shade(fill, -18);
        else if (hover == i)  fill = shade(fill, +14);
        backend->fill_rect(ctx, (float)bx[i], (float)btn_y, (float)bw[i], (float)BTN_H, fill);
        bool def = (i == spec.def_index);
        backend->draw_rect(ctx, (float)bx[i], (float)btn_y, (float)bw[i], (float)BTN_H,
                           def ? 2.0f : 1.0f,
                           def ? C(ColorRole::accent) : C(ColorRole::border));
        int lw = (int)backend->measure_text(ctx, spec.btn[i].label, -1, SZ_TXT);
        backend->draw_text(ctx, (float)(bx[i] + (bw[i] - lw) / 2), (float)btn_y, (float)(lw + 4), (float)BTN_H,
                           spec.btn[i].label, SZ_TXT, C(ColorRole::text_primary));
      }
      backend->end_frame(ctx);
    }
  };

} // namespace neui_detail
