#pragma once

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <neui/d/a11y.h>
#include <neui/d/events.h>   // NEUI_CHECK_*
#include <neui/d/widgets.h>

// Portable accessibility node model.
//
// The xpl host paints one native surface per FRAME, so the platform gives us
// nothing per widget and the accessibility tree has to be synthesised whatever
// granularity we pick. This header is that synthesis: it turns a flat list of
// node candidates into an ordered, parented, pruned tree with roles, names,
// states and bounds resolved - the part that is identical on macOS, Windows and
// Linux, and the part where the bugs live.
//
// IT CONTAINS NO HOST TYPES ON PURPOSE. Everything here works on POD input, so
// the whole model is Tier-1 testable with no window, no platform and no host
// library - the same contract grid_model.h and scrollbar.h honour. The host-side
// adapter that fills the input is where the platform knowledge stays.
//
// THE INPUT IS ONE ROW PER NODE CANDIDATE, NOT PER WIDGET. This is the load-
// bearing design decision. Four widget types contain sub-elements that are model
// state rather than widgets - LISTBOX / COMBOBOX rows, TREEVIEW items, GRID
// rows and cells, TABVIEW chips, MENUBAR items - and a 10000x8 GRID is
// deliberately a single widget. So the adapter emits a self-describing row for
// each of those too, and this model never has to know how any widget stores its
// contents.
//
// Read plans/accessibility.md before changing the shape of any of this.

namespace neui_detail
{
  // What a row represents. `widget` is the widget itself; the rest are
  // sub-elements the owning widget paints.
  enum class A11ySubKind : int32_t
  {
    widget = 0,
    list_row,
    tree_item,
    grid_header,
    grid_row,
    grid_cell,
    tab_chip,
    menu_item
  };

  // Node identity, stable across rebuilds AND across widget-slot reuse.
  //
  // `generation` is why this is not just a widget id. Widget ids are
  // (session << 16 | tree slot) and slots are reused, with no staleness
  // detection anywhere in the framework. Both UI Automation and
  // NSAccessibility hold element references across long spans, so without a
  // generation counter a stale reference to a destroyed widget would silently
  // resolve to whatever widget later took the slot and answer with ITS role,
  // name and bounds - worse than answering "invalid". The adapter bumps the
  // counter on destroy; a11y_find rejects a mismatch.
  struct A11yNodeId
  {
    uint32_t widget_id  = 0;
    uint32_t generation = 0;
    int32_t  sub_kind   = 0;    // A11ySubKind
    int32_t  sub_index  = -1;   // -1 = the widget itself
  };

  inline bool a11y_id_equal(const A11yNodeId& a, const A11yNodeId& b)
  {
    return a.widget_id == b.widget_id && a.generation == b.generation &&
           a.sub_kind == b.sub_kind && a.sub_index == b.sub_index;
  }

  inline bool a11y_id_null(const A11yNodeId& a)
  {
    return a.widget_id == 0 && a.sub_index == -1 && a.sub_kind == 0;
  }

  // One node candidate. Everything the model needs is already resolved here, so
  // the model performs no layout and knows no widget internals.
  struct A11yInput
  {
    A11yNodeId  id;
    A11yNodeId  parent;             // null id = a root of this frame's tree
    const char* type = nullptr;     // NEUI_W_* on widget rows; null on sub-rows

    // Frame-local logical px, ALREADY including section band, scroll offsets and
    // overlay placement - the adapter resolves those, not this model.
    int x = 0, y = 0, w = 0, h = 0;

    // Enclosing scroll / overlay clip, for OFFSCREEN. Absent = not clipped.
    // This is the clip imposed on this row BY AN ANCESTOR, not the one this row
    // imposes on its own children - see `scrollable`.
    bool has_clip = false;
    int  clip_x = 0, clip_y = 0, clip_w = 0, clip_h = 0;

    // This row scrolls its own content, so it is a scroll area rather than a
    // plain container. Kept separate from has_clip on purpose: revision 2 of the
    // model derived "scroll area" from has_clip, which reads plausibly but is
    // wrong in both directions - a scrolling SECTION's own row carries its
    // ANCESTOR's clip (so it looked like a plain group), and a plain SECTION
    // nested inside a scrolling one inherits a clip (so it looked like a scroll
    // area). Two different facts need two different fields.
    bool scrollable = false;

    // Framework-derived state.
    bool visible    = true;
    bool enabled    = true;
    bool focused    = false;
    bool tab_stop   = false;
    bool selected   = false;
    bool expandable = false;
    bool expanded   = false;
    bool readonly   = false;
    bool password   = false;
    bool multiline  = false;
    bool modal_blocked = false;     // input-blocked owner of a modal dialog
    int  check_state   = -1;        // -1 = not checkable, else NEUI_CHECK_*

    // Widget text / row label / cell text / chip label / menu item label.
    const char* text = nullptr;

    // Value.
    bool  has_value = false;
    float value     = 0.0f;         // normalized [0..1] when has_range
    bool  has_range = false;
    float vmin = 0.0f, vmax = 1.0f, vstep = 0.0f;

    // Client declarations, read out of the widget's AttrBag by the adapter.
    int         declared_role = 0;  // NEUI_A11Y_ROLE_DEFAULT = derive
    const char* name = nullptr;
    const char* description = nullptr;
    const char* value_text = nullptr;
    uint32_t    state_mask = 0, state_values = 0;
    A11yNodeId  labelled_by;        // null id = none

    // Virtualized container: when the adapter emits only a window of children
    // (a long GRID / LISTBOX), the container row carries the true totals so a
    // provider can advertise the real set size and index. -1 = not virtualized.
    int total_child_count = -1;
    int first_child_index = 0;
  };

  struct A11yNode
  {
    A11yNodeId  id;
    A11yNodeId  parent;
    std::vector<A11yNodeId> children;   // ordered
    int         role = NEUI_A11Y_ROLE_GROUP;
    std::string name;
    std::string description;
    std::string value_text;
    uint32_t    state = 0;
    int         x = 0, y = 0, w = 0, h = 0;
    int         tab_index = -1;         // -1 = not a tab stop
    int         total_child_count = -1;
    int         first_child_index = 0;
  };

  // ---------------------------------------------------------------------------
  // Role derivation

  inline bool a11y_type_is(const char* type, const char* want)
  {
    if (!type || !want) return false;
    const char* a = type; const char* b = want;
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == '\0' && *b == '\0';
  }

  // The role a row gets when the client declared nothing. Sub-element rows are
  // decided by their kind; widget rows by their NEUI_W_* type.
  inline int a11y_derive_role(const A11yInput& in)
  {
    switch (static_cast<A11ySubKind>(in.id.sub_kind)) {
      case A11ySubKind::list_row:    return NEUI_A11Y_ROLE_LIST_ITEM;
      case A11ySubKind::tree_item:   return NEUI_A11Y_ROLE_TREE_ITEM;
      case A11ySubKind::grid_header: return NEUI_A11Y_ROLE_COLUMN_HEADER;
      case A11ySubKind::grid_row:    return NEUI_A11Y_ROLE_ROW;
      case A11ySubKind::grid_cell:   return NEUI_A11Y_ROLE_CELL;
      case A11ySubKind::tab_chip:    return NEUI_A11Y_ROLE_TAB;
      case A11ySubKind::menu_item:   return NEUI_A11Y_ROLE_MENU_ITEM;
      case A11ySubKind::widget:      break;
    }

    const char* t = in.type;
    if (a11y_type_is(t, NEUI_W_APPWINDOW) || a11y_type_is(t, NEUI_W_PLUGWINDOW) ||
        a11y_type_is(t, NEUI_W_DIALOG))     return NEUI_A11Y_ROLE_WINDOW;
    if (a11y_type_is(t, NEUI_W_LABEL))      return NEUI_A11Y_ROLE_STATIC_TEXT;
    if (a11y_type_is(t, NEUI_W_BUTTON))     return NEUI_A11Y_ROLE_BUTTON;
    if (a11y_type_is(t, NEUI_W_INPUTBOX))   return NEUI_A11Y_ROLE_TEXT_FIELD;
    if (a11y_type_is(t, NEUI_W_MULTILINE))  return NEUI_A11Y_ROLE_TEXT_AREA;
    if (a11y_type_is(t, NEUI_W_CHECKBOX) ||
        a11y_type_is(t, NEUI_W_CHECKBOX3))  return NEUI_A11Y_ROLE_CHECKBOX;
    if (a11y_type_is(t, NEUI_W_LISTBOX))    return NEUI_A11Y_ROLE_LIST;
    if (a11y_type_is(t, NEUI_W_COMBOBOX))   return NEUI_A11Y_ROLE_COMBOBOX;
    if (a11y_type_is(t, NEUI_W_TREEVIEW))   return NEUI_A11Y_ROLE_TREE;
    if (a11y_type_is(t, NEUI_W_GRID))       return NEUI_A11Y_ROLE_TABLE;
    if (a11y_type_is(t, NEUI_W_MENUBAR))    return NEUI_A11Y_ROLE_MENU_BAR;
    if (a11y_type_is(t, NEUI_W_POPUPMENU))  return NEUI_A11Y_ROLE_MENU;
    // KNOB maps to SLIDER: no platform has a "knob" role, and a slider is the
    // contract every AT knows how to drive.
    if (a11y_type_is(t, NEUI_W_SLIDER) ||
        a11y_type_is(t, NEUI_W_KNOB))       return NEUI_A11Y_ROLE_SLIDER;
    if (a11y_type_is(t, NEUI_W_IMAGE))      return NEUI_A11Y_ROLE_IMAGE;
    if (a11y_type_is(t, NEUI_W_TABVIEW))    return NEUI_A11Y_ROLE_TAB_LIST;
    // A scrolling SECTION is a scroll area; a plain one is just a group. The
    // adapter sets `scrollable` from the widget's own scroll state - NOT from
    // has_clip, which is the clip an ancestor imposes and answers a different
    // question (see A11yInput::scrollable).
    if (a11y_type_is(t, NEUI_W_SECTION))
      return in.scrollable ? NEUI_A11Y_ROLE_SCROLL_AREA : NEUI_A11Y_ROLE_GROUP;
    if (a11y_type_is(t, NEUI_W_TABPAGE))    return NEUI_A11Y_ROLE_GROUP;
    // CUSTOMDRAW: deliberately a plain group. See set_role in <neui/d/a11y.h>
    // for why guessing from an attached behavior asset is the wrong move.
    if (a11y_type_is(t, NEUI_W_CUSTOMDRAW)) return NEUI_A11Y_ROLE_GROUP;
    return NEUI_A11Y_ROLE_GROUP;
  }

  // ---------------------------------------------------------------------------
  // Value formatting

  // Trim a fixed-point string: "6.00" -> "6", "-32.30" -> "-32.3".
  inline std::string a11y_trim_zeros(std::string s)
  {
    if (s.find('.') == std::string::npos) return s;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
  }

  inline std::string a11y_format_number(float v)
  {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
    return a11y_trim_zeros(std::string(buf));
  }

  // The string an AT should speak for this row's value. Priority:
  //   1. explicit a11y value text (set_value_text) - always wins
  //   2. NEUI_ATTR_VALUE_TEXT, which the adapter passes in the same field when
  //      no explicit one was set, so a KNOB that already draws a readout gets
  //      accessibility for free
  //   3. a declared real-world range -> map the normalized value onto it
  //   4. a bare normalized value -> a percentage, which at least beats "0.42"
  //   5. no value at all -> empty. A BUTTON must not report a value.
  inline std::string a11y_format_value(const A11yInput& in)
  {
    if (in.value_text && *in.value_text) return std::string(in.value_text);
    if (!in.has_value) return std::string();

    float v = in.value;
    if (!(v == v)) v = 0.0f;                       // NaN
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;

    if (in.has_range) {
      float lo = in.vmin, hi = in.vmax;
      // An inverted range is a caller error, not a reason to emit nonsense:
      // order it and map anyway, so the announced number stays inside the two
      // bounds the client named.
      if (lo > hi) { float t = lo; lo = hi; hi = t; }
      if (!(lo == lo) || !(hi == hi) || lo == hi) return a11y_format_number(lo);
      return a11y_format_number(lo + (hi - lo) * v);
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d %%",
                  static_cast<int>(v * 100.0f + 0.5f));
    return std::string(buf);
  }

  // ---------------------------------------------------------------------------
  // State

  inline uint32_t a11y_derive_state(const A11yInput& in)
  {
    uint32_t s = 0;
    // A modal-blocked frame reports its whole subtree as disabled: offering
    // navigable-but-dead controls is worse than saying they cannot be used.
    if (!in.enabled || in.modal_blocked) s |= NEUI_A11Y_STATE_DISABLED;
    if (in.focused)    s |= NEUI_A11Y_STATE_FOCUSED;
    if (in.tab_stop)   s |= NEUI_A11Y_STATE_FOCUSABLE;
    if (in.selected)   s |= NEUI_A11Y_STATE_SELECTED;
    if (in.expandable) s |= in.expanded ? NEUI_A11Y_STATE_EXPANDED
                                        : NEUI_A11Y_STATE_COLLAPSED;
    if (in.readonly)   s |= NEUI_A11Y_STATE_READONLY;
    if (in.password)   s |= NEUI_A11Y_STATE_PROTECTED;
    if (in.multiline)  s |= NEUI_A11Y_STATE_MULTILINE;
    // Tri-state indeterminate is MIXED, never CHECKED - reporting a "maybe" as
    // a definite "on" is a wrong answer, not a rounding.
    if (in.check_state == NEUI_CHECK_CHECKED)       s |= NEUI_A11Y_STATE_CHECKED;
    else if (in.check_state == NEUI_CHECK_INDETERMINATE) s |= NEUI_A11Y_STATE_MIXED;

    // OFFSCREEN: scrolled outside its clip. Reported rather than pruned, so a
    // focused control that scrolls out of view stays reachable.
    if (in.has_clip) {
      const bool outside = (in.x + in.w <= in.clip_x) ||
                           (in.y + in.h <= in.clip_y) ||
                           (in.x >= in.clip_x + in.clip_w) ||
                           (in.y >= in.clip_y + in.clip_h);
      if (outside) s |= NEUI_A11Y_STATE_OFFSCREEN;
    }
    return s;
  }

  // ---------------------------------------------------------------------------
  // Tree construction

  namespace a11y_impl
  {
    inline bool find_index(const std::vector<A11yInput>& in,
                           const A11yNodeId& id, size_t& out)
    {
      for (size_t i = 0; i < in.size(); ++i)
        if (a11y_id_equal(in[i].id, id)) { out = i; return true; }
      return false;
    }
  }

  // Build the tree. Responsibilities, in order:
  //   - drop rows that cannot be represented (invisible, zero-size)
  //   - resolve roles (declared beats derived)
  //   - prune ROLE_NONE subtrees
  //   - resolve names (declared > labelled_by > text) and drop consumed labels
  //   - compute state (derived, then client overrides applied per mask)
  //   - assign tab_index in input order
  //   - re-parent survivors onto their nearest surviving ancestor, so pruning a
  //     middle node does not orphan its children
  inline std::vector<A11yNode> build_a11y_tree(const std::vector<A11yInput>& in)
  {
    const size_t n = in.size();
    std::vector<int> keep(n, 1);
    // Whether a dropped row also takes its DESCENDANTS with it. The distinction
    // matters and follows the paint walk, which gates PAINTING on size but
    // DESCENT only on visibility (host.cpp:2470 vs :2465):
    //   - invisible  -> subtree goes too; the paint walk does not descend, so
    //                   nothing under it is on screen either
    //   - ROLE_NONE  -> subtree goes too; the client said "decorative", and the
    //                   header documents it as pruning the whole subtree
    //   - zero size  -> ONLY THIS ROW goes. The paint walk still descends, so the
    //                   children are visible on screen, and dropping them would
    //                   hide real controls from an AT. They re-attach to the
    //                   nearest surviving ancestor in pass 4.
    std::vector<int> drop_subtree(n, 0);
    std::vector<int> role(n, NEUI_A11Y_ROLE_GROUP);

    // Pass 1: role + the per-row drop decision.
    for (size_t i = 0; i < n; ++i) {
      const A11yInput& r = in[i];
      role[i] = (r.declared_role != NEUI_A11Y_ROLE_DEFAULT)
                  ? r.declared_role : a11y_derive_role(r);

      if (!r.visible)                     { keep[i] = 0; drop_subtree[i] = 1; continue; }
      if (role[i] == NEUI_A11Y_ROLE_NONE) { keep[i] = 0; drop_subtree[i] = 1; continue; }
      // Zero-size rows have nothing an AT could point at OR speak. A menu model
      // is the documented exception: MENUBAR / POPUPMENU widgets are created 0x0
      // and their items carry the geometry, so dropping the container would
      // delete the menu the role table promises.
      const bool is_menu_container = (role[i] == NEUI_A11Y_ROLE_MENU_BAR ||
                                      role[i] == NEUI_A11Y_ROLE_MENU);
      if ((r.w <= 0 || r.h <= 0) && !is_menu_container) { keep[i] = 0; continue; }
    }

    // Pass 2: propagate subtree drops. Iterate to a fixed point rather than
    // assuming parents precede children in the input - the adapter emits in tree
    // order today, but a model that silently depended on that would break the
    // first time an adapter batched sub-elements. The iteration cap also makes a
    // malformed input with a parent CYCLE terminate instead of hanging the AT.
    for (size_t pass = 0; pass <= n; ++pass) {
      bool changed = false;
      for (size_t i = 0; i < n; ++i) {
        if (drop_subtree[i] || a11y_id_null(in[i].parent)) continue;
        size_t p = 0;
        if (!a11y_impl::find_index(in, in[i].parent, p)) continue;  // frame root
        if (!drop_subtree[p]) continue;
        keep[i] = 0;
        drop_subtree[i] = 1;      // and on down to this row's own descendants
        changed = true;
      }
      if (!changed) break;
    }

    // Pass 3: names, and the labels those names consume.
    std::vector<std::string> names(n);
    std::vector<int> consumed_label(n, 0);
    for (size_t i = 0; i < n; ++i) {
      if (!keep[i]) continue;
      const A11yInput& r = in[i];
      if (r.name && *r.name) { names[i] = r.name; continue; }
      if (!a11y_id_null(r.labelled_by)) {
        size_t li = 0;
        if (a11y_impl::find_index(in, r.labelled_by, li) &&
            !a11y_id_equal(in[li].id, r.id)) {
          const char* lt = in[li].name && *in[li].name ? in[li].name
                                                       : in[li].text;
          if (lt && *lt) {
            names[i] = lt;
            // The label is now spoken as this control's name, so leave it out of
            // the tree - an AT would otherwise read the same words twice. Only
            // drop it if it was a plain label with nothing else to offer.
            if (keep[li] && in[li].id.sub_kind == 0 &&
                role[li] == NEUI_A11Y_ROLE_STATIC_TEXT)
              consumed_label[li] = 1;
            continue;
          }
        }
        // A labelled_by pointing at a pruned, missing or empty target degrades
        // to the widget's own text rather than to an unnamed control.
      }
      if (r.text && *r.text) names[i] = r.text;
    }
    for (size_t i = 0; i < n; ++i) if (consumed_label[i]) keep[i] = 0;

    // Pass 4: emit, re-parenting each survivor onto its nearest surviving
    // ancestor. Without this, pruning a decorative middle node would orphan
    // everything under it - the single most likely way to crash a provider.
    std::vector<A11yNode> out;
    out.reserve(n);
    std::vector<size_t> out_index(n, static_cast<size_t>(-1));
    int next_tab = 0;

    for (size_t i = 0; i < n; ++i) {
      if (!keep[i]) continue;
      const A11yInput& r = in[i];

      A11yNode node;
      node.id   = r.id;
      node.role = role[i];
      node.name = names[i];
      if (r.description && *r.description) node.description = r.description;
      node.value_text = a11y_format_value(r);
      node.x = r.x; node.y = r.y; node.w = r.w; node.h = r.h;
      node.total_child_count = r.total_child_count;
      node.first_child_index = r.first_child_index;

      uint32_t st = a11y_derive_state(r);
      if (r.state_mask) {
        st &= ~r.state_mask;
        st |= (r.state_values & r.state_mask);
      }
      node.state = st;

      // Walk up to the nearest ancestor that survived.
      A11yNodeId parent{};
      A11yNodeId cursor = r.parent;
      for (size_t guard = 0; guard <= n && !a11y_id_null(cursor); ++guard) {
        size_t p = 0;
        if (!a11y_impl::find_index(in, cursor, p)) break;   // frame root
        if (keep[p]) { parent = in[p].id; break; }
        cursor = in[p].parent;
      }
      node.parent = parent;

      if (r.tab_stop && r.visible && r.enabled && !r.modal_blocked)
        node.tab_index = next_tab++;

      out_index[i] = out.size();
      out.push_back(node);
    }

    // Pass 5: children, in input order, from the parents just resolved.
    for (size_t oi = 0; oi < out.size(); ++oi) {
      if (a11y_id_null(out[oi].parent)) continue;
      for (size_t pj = 0; pj < out.size(); ++pj) {
        if (a11y_id_equal(out[pj].id, out[oi].parent)) {
          out[pj].children.push_back(out[oi].id);
          break;
        }
      }
    }
    return out;
  }

  // Resolve a node id. A stale `generation` deliberately finds NOTHING, even
  // when a live widget occupies that slot - see A11yNodeId.
  inline const A11yNode* a11y_find(const std::vector<A11yNode>& nodes,
                                   const A11yNodeId& id)
  {
    for (const auto& nd : nodes)
      if (a11y_id_equal(nd.id, id)) return &nd;
    return nullptr;
  }

  // Topmost, innermost node containing (x, y) in frame-local logical px.
  // Offscreen nodes are skipped: they are in the tree so a focused control that
  // scrolls away stays reachable, but they are not at their reported position.
  inline const A11yNode* a11y_hit_test(const std::vector<A11yNode>& nodes,
                                       int x, int y)
  {
    const A11yNode* best = nullptr;
    long best_area = 0;
    for (const auto& nd : nodes) {
      if (nd.state & NEUI_A11Y_STATE_OFFSCREEN) continue;
      if (x < nd.x || y < nd.y || x >= nd.x + nd.w || y >= nd.y + nd.h) continue;
      const long area = static_cast<long>(nd.w) * static_cast<long>(nd.h);
      // Innermost = smallest containing box. Ties go to the LATER node, which is
      // the one painted on top.
      if (!best || area <= best_area) { best = &nd; best_area = area; }
    }
    return best;
  }
}
