#pragma once

#include <cstdint>
#include <vector>

#include "../shared/a11y_tree.h"
#include "host.h"

// Accessibility adapter: turns one FRAME's live widget tree into the flat
// A11yInput rows that hosts/shared/a11y_tree.h consumes.
//
// This is the platform-knowledge half of the accessibility wave. The model it
// feeds is portable and Tier-1 tested with no host at all; everything that
// requires knowing how a neui widget stores its contents lives here. The split
// is worth stating because it is where the coverage sits: the model has 46 unit
// tests, this file has none and cannot have any (it needs host types, a session
// and a realized frame), so its coverage is the macOS smoke harness only. On
// win32 and Linux this is shared code exercised solely through providers that
// cannot be run on the machine in the loop.
//
// THE ROWS ARE NOT ONE-PER-WIDGET. Four widget types keep their contents as
// paint state rather than as child widgets - LISTBOX / COMBOBOX rows, TREEVIEW
// items, GRID headers / rows / cells, TABVIEW chips - and a MENUBAR keeps a menu
// item model. A 10000x8 GRID being a single widget is deliberate, so the adapter
// emits a sub-element row per visible piece instead, and the model never learns
// how any of it is stored.
//
// LIFETIME. Every `const char*` on an emitted row points into live host storage
// (WidgetData::text, an AttrBag, GridModel::rows, MenubarWidget::menu_items) -
// nothing is copied. The rows are therefore valid only until the next tree or
// model mutation, which is fine for the one supported usage: collect, build,
// discard. Do not stash them.
namespace xpl_host
{
  // The node id for a widget itself, stamped with the slot's current
  // generation. Use this to hand a provider the id for a given widget.
  neui_detail::A11yNodeId a11y_widget_node_id(Session& s, uint32_t widget_index);

  // Tree slot named by an A11yNodeId, or 0 when the id is stale / foreign.
  // Rejects a generation mismatch, which is the whole point of the generation.
  uint32_t a11y_slot_of_node_id(Session& s, const neui_detail::A11yNodeId& id);

  // Collect the input rows for `frame_index`, appending to `out`.
  //
  // Returns false - having appended nothing - when the frame is unknown, is not
  // a frame, or has NEVER PAINTED (so the SECTION / TABVIEW body rects and
  // TABVIEW chip rects the walk needs do not exist yet). A caller that gets
  // false must not fall back to reporting positions of its own: an AT pointing a
  // magnifier at the wrong place is worse than an AT told "not available".
  //
  // True does NOT promise the geometry is fresh. A frame that has painted before
  // but whose tree changed since, and whose forced repaint could not happen
  // (hidden / unmapped window), reports its LAST-PAINTED layout - stale beats
  // wrong is the documented contract of Session::ensure_abs_positions, and the
  // staleness is not detectable from here.
  //
  // MUST NOT be called from inside a paint - ensure_abs_positions can force one.
  bool a11y_collect_frame_inputs(Session& s, uint32_t frame_index,
                                 std::vector<neui_detail::A11yInput>& out);

  // a11y_collect_frame_inputs + build_a11y_tree. The normal entry point for a
  // platform provider; empty vector on the failure above.
  std::vector<neui_detail::A11yNode> a11y_build_frame_tree(Session& s,
                                                           uint32_t frame_index);

  // Same, resolving the session from a public frame handle. The platform
  // providers already hold a Session*, so this exists for the accessibility
  // harness (and any future tooling) which only has public handles. Empty
  // vector for an unknown / non-frame handle.
  std::vector<neui_detail::A11yNode> a11y_build_tree_for_frame(neui_widget_t frame);
}
