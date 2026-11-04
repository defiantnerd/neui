#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Damage region for partial repaint: the set of frame-local rectangles that
// must be re-rasterised and re-presented on the next frame. Logical pixels
// at 96 DPI, the same space as a widget's cached abs_x / abs_y (so a damage
// rect built from those lands exactly where the paint walk will draw).
//
// A region is either EMPTY (nothing to repaint), FULL (repaint everything -
// the pre-damage behaviour, and the safe fallback for any change whose
// extent is not known) or a short list of disjoint rects. add() merges a
// new rect with every rect it overlaps or nearly touches, then caps the list
// at k_max_rects by merging the pair whose union wastes the least area -
// many small rects cost more per-rect overhead (clip setup, one blit each)
// than the over-painted gap between them.
//
// Used by the xpl host (Session::paint_frame / the paint-walk cull) and the
// Linux platform (per-window pending damage). See plans/xpl-partial-repaint.md.

namespace neui_detail
{
  struct DamageRect
  {
    int x = 0, y = 0, w = 0, h = 0;
  };

  inline bool damage_rect_empty(const DamageRect& r)
  {
    return r.w <= 0 || r.h <= 0;
  }

  inline int64_t damage_rect_area(const DamageRect& r)
  {
    return damage_rect_empty(r) ? 0 : static_cast<int64_t>(r.w) * r.h;
  }

  inline DamageRect damage_rect_union(const DamageRect& a, const DamageRect& b)
  {
    if (damage_rect_empty(a)) return b;
    if (damage_rect_empty(b)) return a;
    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    return { x0, y0, x1 - x0, y1 - y0 };
  }

  // True when a and b share interior area, or come within `gap` px of each
  // other on both axes (gap 0 = strict overlap; touching edges don't count).
  inline bool damage_rects_near(const DamageRect& a, const DamageRect& b, int gap)
  {
    if (damage_rect_empty(a) || damage_rect_empty(b)) return false;
    return a.x < b.x + b.w + gap && b.x < a.x + a.w + gap &&
           a.y < b.y + b.h + gap && b.y < a.y + a.h + gap;
  }

  class DamageRegion
  {
  public:
    // At most this many rects are kept; beyond it the cheapest pair merges.
    static constexpr std::size_t k_max_rects = 8;
    // Rects closer than this (logical px) merge on add - one slightly larger
    // rect beats two blits separated by a sliver.
    static constexpr int k_merge_gap = 8;

    bool empty() const { return !_full && _rects.empty(); }
    bool full()  const { return _full; }
    const std::vector<DamageRect>& rects() const { return _rects; }

    void clear()     { _full = false; _rects.clear(); }
    void mark_full() { _full = true;  _rects.clear(); }

    void add(const DamageRect& r)
    {
      if (_full || damage_rect_empty(r)) return;
      DamageRect cur = r;
      // Absorb every rect the (growing) new rect is near; repeat until the
      // union stops growing into further neighbours.
      bool merged = true;
      while (merged) {
        merged = false;
        for (std::size_t i = 0; i < _rects.size(); ++i) {
          if (damage_rects_near(cur, _rects[i], k_merge_gap)) {
            cur = damage_rect_union(cur, _rects[i]);
            _rects.erase(_rects.begin() + static_cast<std::ptrdiff_t>(i));
            merged = true;
            break;
          }
        }
      }
      _rects.push_back(cur);
      while (_rects.size() > k_max_rects) merge_cheapest_pair();
    }

    DamageRect bounds() const
    {
      DamageRect b{};
      for (const auto& r : _rects) b = damage_rect_union(b, r);
      return b;
    }

    // Does r need repainting? A FULL region intersects everything.
    bool intersects(const DamageRect& r) const
    {
      if (_full) return true;
      for (const auto& d : _rects)
        if (damage_rects_near(d, r, 0)) return true;
      return false;
    }

    // Covered area in px^2. The rects are kept disjoint (overlapping or
    // near rects always merge), so the plain sum is exact.
    int64_t area() const
    {
      int64_t a = 0;
      for (const auto& r : _rects) a += damage_rect_area(r);
      return a;
    }

    // Fraction of a frame_w x frame_h frame the damage covers (1 when FULL;
    // rects are clipped to the frame first). Callers repaint the whole
    // frame above a threshold - past ~60% the per-rect overhead outweighs
    // the pixels saved.
    float coverage(int frame_w, int frame_h) const
    {
      if (_full) return 1.0f;
      if (frame_w <= 0 || frame_h <= 0) return 1.0f;
      int64_t covered = 0;
      for (const auto& r : _rects) {
        int x0 = r.x < 0 ? 0 : r.x;
        int y0 = r.y < 0 ? 0 : r.y;
        int x1 = (r.x + r.w) > frame_w ? frame_w : (r.x + r.w);
        int y1 = (r.y + r.h) > frame_h ? frame_h : (r.y + r.h);
        if (x1 > x0 && y1 > y0)
          covered += static_cast<int64_t>(x1 - x0) * (y1 - y0);
      }
      return static_cast<float>(covered) /
             (static_cast<float>(frame_w) * static_cast<float>(frame_h));
    }

  private:
    void merge_cheapest_pair()
    {
      std::size_t bi = 0, bj = 1;
      int64_t best = -1;
      for (std::size_t i = 0; i < _rects.size(); ++i) {
        for (std::size_t j = i + 1; j < _rects.size(); ++j) {
          int64_t waste = damage_rect_area(damage_rect_union(_rects[i], _rects[j]))
                        - damage_rect_area(_rects[i]) - damage_rect_area(_rects[j]);
          if (best < 0 || waste < best) { best = waste; bi = i; bj = j; }
        }
      }
      DamageRect u = damage_rect_union(_rects[bi], _rects[bj]);
      _rects.erase(_rects.begin() + static_cast<std::ptrdiff_t>(bj));
      _rects.erase(_rects.begin() + static_cast<std::ptrdiff_t>(bi));
      // The union may now reach other rects - re-add so disjointness holds.
      add(u);
    }

    bool                    _full = false;
    std::vector<DamageRect> _rects;
  };

} // namespace neui_detail
