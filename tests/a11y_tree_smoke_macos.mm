// Accessibility ADAPTER acceptance harness (xpl host, macOS).
//
// tests/test_a11y_tree.cpp covers the portable model with fabricated input rows.
// This covers the other half: the host adapter that produces those rows from a
// LIVE widget tree. That half cannot be Tier-1 tested (it needs host types, a
// session and a realized frame), so this harness is its only coverage - and on
// win32 and Linux the adapter is shared code with no coverage at all, exercised
// solely through providers that cannot be run on this machine.
//
// The check that earns its place is #3, GEOMETRY vs HIT-TEST. Every other
// assertion here compares the adapter against the adapter's own bookkeeping, so
// a systematically wrong origin would keep them all agreeing - exactly the trap
// the focus harness hit. So the reported rectangle of a widget nested inside a
// chip-bearing SECTION is cross-validated by POSTING A REAL CLICK at its centre
// and asserting the production hit-test resolves the same widget: two
// independent code paths, one answer. If the adapter and the paint walk ever
// disagree about where a widget is, a screen reader would point its magnifier
// somewhere the user cannot click, and this is what catches it.
//
// The other things worth pinning down, each a way the adapter could be wrong
// without any unit test noticing:
//   1  SHAPE        - tree exists, rooted at the frame as a WINDOW.
//   2  PARENTAGE    - every node's parent is present; no orphans reach a provider.
//   3  GEOMETRY     - see above.
//   4  ROLES        - derived from real widget types, incl. plain vs scrolling
//                     SECTION (group vs scroll area).
//   5  LIST ROWS    - one node per VISIBLE row, right text, right selection.
//   6  VIRTUALIZED  - 100 items produce a handful of nodes but report 100 total.
//   7  GRID         - headers + windowed rows/cells, text matching the model.
//   8  OFFSCREEN    - a child scrolled out of a SECTION is reported, not dropped.
//   9  GENERATION   - an id held across destroy + slot reuse resolves to NOTHING.
//  10  LABELLED_BY  - the input takes the label's text and the label leaves.
//  11  ROLE_NONE    - prunes the declared node AND its subtree.
//  12  ZERO SIZE    - drops only that node; its visible child survives, reparented.
//  13  TABVIEW      - one TAB per page, selected chip marked, hidden page pruned.
//  14  COMBOBOX     - closed reports totals but no rows; open positions rows on
//                     the overlay rect rather than on the collapsed bar.
//  15  VALUE        - a declared range turns a normalized knob into a real number.
//
// Realizes two real NSWindows, so built but NOT ctest-registered; run
// ./tests/<config>/neui_a11y_tree_smoke_macos manually.

#import <AppKit/AppKit.h>

#include <neui/neui.h>

#include "a11y_adapter.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using neui_detail::A11yNode;
using neui_detail::A11yNodeId;
using neui_detail::A11ySubKind;

namespace {

int g_failures = 0;

uint32_t g_mouse_widget = 0;
int      g_mouse_x = -1, g_mouse_y = -1;

void check(bool ok, const char* what)
{
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) ++g_failures;
}

void check_eq_int(long got, long want, const char* what)
{
  bool ok = (got == want);
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) { std::printf("        want %ld, got %ld\n", want, got); ++g_failures; }
}

void check_eq_str(const std::string& got, const char* want, const char* what)
{
  bool ok = (got == want);
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) {
    std::printf("        want \"%s\", got \"%s\"\n", want, got.c_str());
    ++g_failures;
  }
}

bool onevent(void*, neui_event_t* ev)
{
  if (ev->type == NEUI_EVENT_MOUSE_BUTTON_DOWN) {
    g_mouse_widget = ev->data.mouse.widget.id;
    g_mouse_x      = ev->data.mouse.x;
    g_mouse_y      = ev->data.mouse.y;
  }
  return false;
}

neui_widget_client_t g_wc = { NEUI_VERSION, nullptr, onevent };
void* iface(void*, const char* n)
{ return std::strcmp(n, NEUI_API_WIDGETS) ? nullptr : (void*)&g_wc; }
neui_client_t g_client = { NEUI_VERSION, iface };

// ---- tree lookup helpers ---------------------------------------------------

const A11yNode* find_widget(const std::vector<A11yNode>& t, neui_widget_t w)
{
  for (const auto& nd : t)
    if (nd.id.widget_id == w.id &&
        nd.id.sub_kind == (int32_t)A11ySubKind::widget && nd.id.sub_index == -1)
      return &nd;
  return nullptr;
}

const A11yNode* find_sub(const std::vector<A11yNode>& t, neui_widget_t w,
                         A11ySubKind kind, int32_t sub)
{
  for (const auto& nd : t)
    if (nd.id.widget_id == w.id && nd.id.sub_kind == (int32_t)kind &&
        nd.id.sub_index == sub)
      return &nd;
  return nullptr;
}

int count_sub(const std::vector<A11yNode>& t, neui_widget_t w, A11ySubKind kind)
{
  int n = 0;
  for (const auto& nd : t)
    if (nd.id.widget_id == w.id && nd.id.sub_kind == (int32_t)kind) ++n;
  return n;
}

NSWindow* window_titled(NSString* title)
{
  for (NSWindow* win in [NSApp windows])
    if ([[win title] isEqualToString:title]) return win;
  return nil;
}

void click_in(NSWindow* win, float lx, float ly)
{
  NSView* v = [win contentView];
  if (!v) return;
  // locationInWindow is y-up from the bottom-left; the view is isFlipped:YES.
  const CGFloat h = [v bounds].size.height;
  NSEvent* ev = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                   location:NSMakePoint(lx, h - ly)
                              modifierFlags:0
                                  timestamp:0
                               windowNumber:[win windowNumber]
                                    context:nil
                                eventNumber:0
                                 clickCount:1
                                   pressure:1.0];
  [v mouseDown:ev];
}

// kVK_* keycodes go through mac_keycode_to_neui in the platform layer, so this
// drives the production key path rather than poking widget state.
void post_key(NSWindow* win, unsigned short kvk)
{
  NSView* v = [win contentView];
  if (!v) return;
  NSEvent* ev = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                 location:NSZeroPoint
                            modifierFlags:0
                                timestamp:0
                             windowNumber:[win windowNumber]
                                  context:nil
                               characters:@""
              charactersIgnoringModifiers:@""
                                isARepeat:NO
                                  keyCode:kvk];
  [v keyDown:ev];
}

void pump(double seconds)
{
  [[NSRunLoop mainRunLoop] runUntilDate:
      [NSDate dateWithTimeIntervalSinceNow:seconds]];
}

} // namespace

int main()
{
  @autoreleasepool {
    [NSApplication sharedApplication];

    neui_init();
    neui_api_t* api = neui_get_api("neui.host.crossplatform");
    if (!api) { std::printf("FAIL: no crossplatform host\n"); return 1; }

    void* app = nullptr;
    neui_session_t sess = api->create_session(&g_client, &app);
    auto* w     = (neui_widget_api_t*) api->get_interface(sess, NEUI_API_WIDGETS);
    auto* attrs = (neui_attr_api_t*)   api->get_interface(sess, NEUI_API_ATTRS);
    auto* items = (neui_items_api_t*)  api->get_interface(sess, NEUI_API_ITEMS);
    auto* grid  = (neui_grid_api_t*)   api->get_interface(sess, NEUI_API_GRID);
    auto* a11y  = (neui_a11y_api_t*)   api->get_interface(sess, NEUI_API_A11Y);
    auto* tree_api = (neui_tree_api_t*) api->get_interface(sess, NEUI_API_TREE);
    if (!w || !attrs || !items || !grid || !a11y || !tree_api) {
      std::printf("FAIL: missing interface\n"); return 1;
    }

    // ---------------------------------------------------------------------
    // Frame A - the mainstream cases. Sized from the laid-out content
    // (max child extent 240 x 304) plus a margin, per the house rule.
    neui_widget_t fa = w->create(sess, widget_none, NEUI_W_APPWINDOW,
                                 60, 100, 260, 320, nullptr);
    w->set_text(sess, fa, "a11y A");

    neui_widget_t lbl = w->create(sess, fa, NEUI_W_LABEL, 12, 12, 80, 20, nullptr);
    w->set_text(sess, lbl, "Cutoff");
    neui_widget_t inp = w->create(sess, fa, NEUI_W_INPUTBOX, 100, 12, 140, 22, nullptr);
    // Check 10: the LABEL names the INPUTBOX. A layout convention the framework
    // cannot see, which is exactly what set_labelled_by is for.
    a11y->set_labelled_by(sess, inp, lbl);

    neui_widget_t knob = w->create(sess, fa, NEUI_W_KNOB, 12, 44, 60, 60, nullptr);
    attrs->set_float(sess, knob, NEUI_PARAM_VALUE, 0.5f);
    // Check 15: -60..+6 dB, so 0.5 must announce as -27, not "50 %".
    a11y->set_value_range(sess, knob, -60.0f, 6.0f, 0.0f);

    neui_widget_t list = w->create(sess, fa, NEUI_W_LISTBOX, 100, 44, 140, 60, nullptr);
    for (int i = 0; i < 100; ++i) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "item %d", i);
      items->add(sess, list, buf, nullptr);
    }
    items->set_selected(sess, list, 1);

    neui_widget_t sect = w->create(sess, fa, NEUI_W_SECTION, 12, 116, 228, 80, nullptr);
    w->set_text(sess, sect, "Sect");
    neui_widget_t sbtn = w->create(sess, sect, NEUI_W_BUTTON, 10, 10, 100, 26, nullptr);
    w->set_text(sess, sbtn, "inner");

    neui_widget_t gr = w->create(sess, fa, NEUI_W_GRID, 12, 204, 228, 100, nullptr);
    grid->add_column(sess, gr, "Name", 90);
    grid->add_column(sess, gr, "Kind", 70);
    grid->add_column(sess, gr, "Val",  70);
    for (int r = 0; r < 50; ++r) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "row%d", r);
      const char* vals[3] = { buf, "kind", "42" };
      grid->add_row(sess, gr, vals);
    }
    w->show(sess, fa);

    // ---------------------------------------------------------------------
    // Frame B - the pruning / overlay / scrolling cases. Content max 212 x 324.
    neui_widget_t fb = w->create(sess, widget_none, NEUI_W_APPWINDOW,
                                 360, 40, 230, 496, nullptr);
    w->set_text(sess, fb, "a11y B");

    // 11: declared decorative - this section AND its button must disappear.
    neui_widget_t deco = w->create(sess, fb, NEUI_W_SECTION, 12, 12, 200, 60, nullptr);
    a11y->set_role(sess, deco, NEUI_A11Y_ROLE_NONE);
    neui_widget_t deco_child = w->create(sess, deco, NEUI_W_BUTTON, 10, 10, 100, 26, nullptr);
    w->set_text(sess, deco_child, "hidden");

    // 12: zero-size container. The paint walk still DESCENDS into it, so its
    // child is genuinely on screen - only the container itself may be dropped.
    neui_widget_t zero = w->create(sess, fb, NEUI_W_SECTION, 12, 84, 0, 0, nullptr);
    neui_widget_t zero_child = w->create(sess, zero, NEUI_W_BUTTON, 0, 0, 100, 26, nullptr);
    w->set_text(sess, zero_child, "survivor");

    // 8: a scrolling SECTION with more content than fits.
    neui_widget_t scr = w->create(sess, fb, NEUI_W_SECTION, 12, 120, 200, 60, nullptr);
    attrs->set_string(sess, scr, NEUI_ATTR_SCROLL_MODE, "vertical");
    attrs->set_string(sess, scr, NEUI_ATTR_ALIGN_TEXT, "none");
    neui_widget_t scr_first{}, scr_last{};
    for (int i = 0; i < 6; ++i) {
      neui_widget_t b = w->create(sess, scr, NEUI_W_BUTTON, 8, 6 + i * 34,
                                  120, 26, nullptr);
      char buf[16];
      std::snprintf(buf, sizeof(buf), "s%d", i);
      w->set_text(sess, b, buf);
      if (i == 0) scr_first = b;
      if (i == 5) scr_last  = b;
    }

    // 16: a TREEVIEW that will be SCROLLED so its window starts below depth 0 -
    // the parent of every visible row is then outside the window. That is the
    // case that used to orphan tree items onto the frame, so it needs a real
    // scroll position rather than a synthetic one.
    neui_widget_t tree = w->create(sess, fb, NEUI_W_TREEVIEW, 12, 344, 200, 80, nullptr);
    neui_item_t troot = tree_api->add(sess, tree, tree_item_root, "Group", nullptr);
    for (int i = 0; i < 12; ++i) {
      char buf[24];
      std::snprintf(buf, sizeof(buf), "kid %d", i);
      tree_api->add(sess, tree, troot, buf, nullptr);
    }

    // 14: a COMBOBOX, closed for now.
    neui_widget_t combo = w->create(sess, fb, NEUI_W_COMBOBOX, 12, 192, 150, 22, nullptr);
    items->add(sess, combo, "alpha", nullptr);
    items->add(sess, combo, "beta",  nullptr);
    items->add(sess, combo, "gamma", nullptr);
    items->set_selected(sess, combo, 0);

    // 13: a TABVIEW with two pages; only the selected page's subtree is live.
    neui_widget_t tv = w->create(sess, fb, NEUI_W_TABVIEW, 12, 224, 200, 100, nullptr);
    neui_widget_t p1 = w->create(sess, tv, NEUI_W_TABPAGE, 0, 0, 0, 0, nullptr);
    w->set_text(sess, p1, "One");
    neui_widget_t p1b = w->create(sess, p1, NEUI_W_BUTTON, 8, 8, 100, 26, nullptr);
    w->set_text(sess, p1b, "on page one");
    neui_widget_t p2 = w->create(sess, tv, NEUI_W_TABPAGE, 0, 0, 0, 0, nullptr);
    w->set_text(sess, p2, "Two");
    neui_widget_t p2b = w->create(sess, p2, NEUI_W_BUTTON, 8, 8, 100, 26, nullptr);
    w->set_text(sess, p2b, "on page two");

    // 17: a COMBOBOX inside a SCROLLING section, low enough that its open drop
    // list extends past the section body. Session::paint_frame paints the overlay
    // after the widget walk with every clip popped, so those rows really are on
    // screen and clickable - the adapter must not report them clipped by the
    // section, or a11y_hit_test would skip them as OFFSCREEN.
    neui_widget_t scr2 = w->create(sess, fb, NEUI_W_SECTION, 12, 436, 200, 44, nullptr);
    attrs->set_string(sess, scr2, NEUI_ATTR_SCROLL_MODE, "vertical");
    attrs->set_string(sess, scr2, NEUI_ATTR_ALIGN_TEXT, "none");
    neui_widget_t combo2 = w->create(sess, scr2, NEUI_W_COMBOBOX, 8, 8, 140, 22, nullptr);
    for (int i = 0; i < 6; ++i) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "opt %d", i);
      items->add(sess, combo2, buf, nullptr);
    }
    items->set_selected(sess, combo2, 0);
    // Filler so the section really does scroll (and so really does clip).
    w->create(sess, scr2, NEUI_W_BUTTON, 8, 40, 120, 26, nullptr);

    w->show(sess, fb);

    // Both frames must have painted: SECTION / TABVIEW body rects and TABVIEW
    // chip rects are produced by paint, and the whole walk depends on them.
    pump(0.35);

    NSWindow* wa = window_titled(@"a11y A");
    NSWindow* wb = window_titled(@"a11y B");
    check(wa != nil && wb != nil, "both frames realized as NSWindows");
    if (!wa || !wb) { std::printf("\nA11Y TREE FAILED (setup)\n"); return 1; }

    // =====================================================================
    std::printf("\n-- frame A --\n");
    auto ta = xpl_host::a11y_build_tree_for_frame(fa);

    // 1  SHAPE
    check(!ta.empty(), "1  frame A produces a non-empty tree");
    const A11yNode* root = find_widget(ta, fa);
    check(root != nullptr, "1  the frame itself is a node");
    if (root) {
      check_eq_int(root->role, NEUI_A11Y_ROLE_WINDOW, "1  frame role is WINDOW");
      check(root->parent.widget_id == 0, "1  the frame is a root (null parent)");
    }

    // 2  PARENTAGE - no orphans. A provider that follows a dangling parent id
    //    is the single most likely way to crash an AT.
    {
      bool all_parents_present = true;
      for (const auto& nd : ta) {
        if (nd.parent.widget_id == 0 && nd.parent.sub_index == -1 &&
            nd.parent.sub_kind == 0)
          continue;                                     // a root
        if (!neui_detail::a11y_find(ta, nd.parent)) { all_parents_present = false; break; }
      }
      check(all_parents_present, "2  every node's parent is present in the tree");

      // And the inverse: a parent lists exactly the children that name it.
      bool inverse_ok = true;
      for (const auto& nd : ta) {
        for (const auto& kid : nd.children) {
          const A11yNode* k = neui_detail::a11y_find(ta, kid);
          if (!k || !neui_detail::a11y_id_equal(k->parent, nd.id)) {
            inverse_ok = false; break;
          }
        }
      }
      check(inverse_ok, "2  children lists are the inverse of parent links");
    }

    // 3  GEOMETRY vs HIT-TEST - the cross-validation.
    const A11yNode* n_sbtn = find_widget(ta, sbtn);
    check(n_sbtn != nullptr, "3  the section's inner button is a node");
    if (n_sbtn) {
      // The section carries a header chip, so a correct y is BELOW the band -
      // if the adapter forgot the body offset it would report the chip band.
      check(n_sbtn->y > 116, "3  inner button y is below the section's chip band");
      const int cx = n_sbtn->x + n_sbtn->w / 2;
      const int cy = n_sbtn->y + n_sbtn->h / 2;
      g_mouse_widget = 0;
      click_in(wa, (float)cx, (float)cy);
      pump(0.05);
      check(g_mouse_widget == sbtn.id,
            "3  a click at the reported centre hits the reported widget");
      if (g_mouse_widget != sbtn.id)
        std::printf("        reported rect (%d,%d %dx%d) -> hit widget id %u\n",
                    n_sbtn->x, n_sbtn->y, n_sbtn->w, n_sbtn->h, g_mouse_widget);
      // And the coordinates the widget received are widget-local, which only
      // works if the adapter's origin and the dispatcher's agree exactly.
      check_eq_int(g_mouse_x, n_sbtn->w / 2, "3  widget-local x matches the reported rect");
      check_eq_int(g_mouse_y, n_sbtn->h / 2, "3  widget-local y matches the reported rect");
    }

    // 4  ROLES from real widget types.
    if (const A11yNode* n = find_widget(ta, inp))
      check_eq_int(n->role, NEUI_A11Y_ROLE_TEXT_FIELD, "4  INPUTBOX -> TEXT_FIELD");
    if (const A11yNode* n = find_widget(ta, knob))
      check_eq_int(n->role, NEUI_A11Y_ROLE_SLIDER, "4  KNOB -> SLIDER");
    if (const A11yNode* n = find_widget(ta, list))
      check_eq_int(n->role, NEUI_A11Y_ROLE_LIST, "4  LISTBOX -> LIST");
    if (const A11yNode* n = find_widget(ta, gr))
      check_eq_int(n->role, NEUI_A11Y_ROLE_TABLE, "4  GRID -> TABLE");
    if (const A11yNode* n = find_widget(ta, sect))
      check_eq_int(n->role, NEUI_A11Y_ROLE_GROUP,
                   "4  non-scrolling SECTION -> GROUP");

    // 5  LIST ROWS
    {
      const int rows = count_sub(ta, list, A11ySubKind::list_row);
      check(rows > 0 && rows < 100,
            "5  a 100-item list emits only its visible rows");
      // 60px box / 18px rows = 3 whole rows plus a partial 4th. paint_scrollable_
      // list uses CEILING division, and that partial row is clickable, so the
      // adapter must emit 4 - a floor would hide a row the user can select.
      check_eq_int(rows, 4, "5  incl. the partially visible trailing row (ceil)");
      const A11yNode* r0 = find_sub(ta, list, A11ySubKind::list_row, 0);
      check(r0 != nullptr, "5  row 0 is a node");
      if (r0) {
        check_eq_str(r0->name, "item 0", "5  row 0 carries its item text");
        check(neui_detail::a11y_id_equal(r0->parent, find_widget(ta, list)->id),
              "5  rows parent to the list");
      }
      const A11yNode* r1 = find_sub(ta, list, A11ySubKind::list_row, 1);
      check(r1 && (r1->state & NEUI_A11Y_STATE_SELECTED),
            "5  the selected row reports SELECTED");
      if (r0)
        check(!(r0->state & NEUI_A11Y_STATE_SELECTED),
              "5  an unselected row does not");
    }

    // 6  VIRTUALIZED totals - so a provider can advertise the real set size.
    if (const A11yNode* n = find_widget(ta, list)) {
      check_eq_int(n->total_child_count, 100, "6  list reports 100 total children");
      check_eq_int(n->first_child_index, 0, "6  list reports its scroll offset");
    }

    // 7  GRID headers + windowed cells.
    {
      const A11yNode* h0 = find_sub(ta, gr, A11ySubKind::grid_header, 0);
      check(h0 != nullptr, "7  column 0 has a header node");
      if (h0) {
        check_eq_int(h0->role, NEUI_A11Y_ROLE_COLUMN_HEADER, "7  header role");
        check_eq_str(h0->name, "Name", "7  header carries the column title");
      }
      const int grid_rows = count_sub(ta, gr, A11ySubKind::grid_row);
      check(grid_rows > 0 && grid_rows < 50,
            "7  a 50-row grid emits only its visible rows");
      if (const A11yNode* n = find_widget(ta, gr))
        check_eq_int(n->total_child_count, 50, "7  grid reports 50 total rows");
      const A11yNode* row0 = find_sub(ta, gr, A11ySubKind::grid_row, 0);
      check(row0 != nullptr, "7  logical row 0 is a node");
      if (row0) {
        check_eq_int(row0->role, NEUI_A11Y_ROLE_ROW, "7  row role");
        check_eq_str(row0->name, "row0",
                     "7  a row is named by its first cell");
      }
      // Cell (0, 1) - packed sub_index, parented to its ROW not the table.
      const A11yNode* c01 = find_sub(ta, gr, A11ySubKind::grid_cell, 0 * 1024 + 1);
      check(c01 != nullptr, "7  cell (0,1) is a node");
      if (c01 && row0) {
        check_eq_int(c01->role, NEUI_A11Y_ROLE_CELL, "7  cell role");
        check_eq_str(c01->name, "kind", "7  cell carries its own text");
        check(neui_detail::a11y_id_equal(c01->parent, row0->id),
              "7  a cell parents to its row, not to the table");
        // Columns are not editable by default, so every cell is read-only -
        // saying so keeps an AT from offering an edit that cannot commit.
        check(c01->state & NEUI_A11Y_STATE_READONLY,
              "7  a non-editable column's cells report READONLY");
      }
    }

    // 10  LABELLED_BY
    {
      const A11yNode* n = find_widget(ta, inp);
      check(n != nullptr, "10 the input is a node");
      if (n) check_eq_str(n->name, "Cutoff", "10 input takes the label's text");
      check(find_widget(ta, lbl) == nullptr,
            "10 the consumed LABEL is dropped (no double announcement)");
    }

    // 15  VALUE mapped onto the declared range.
    if (const A11yNode* n = find_widget(ta, knob))
      check_eq_str(n->value_text, "-27",
                   "15 a normalized 0.5 over -60..6 announces as -27");

    // =====================================================================
    std::printf("\n-- frame B --\n");
    auto tb = xpl_host::a11y_build_tree_for_frame(fb);
    check(!tb.empty(), "   frame B produces a non-empty tree");

    // 11  ROLE_NONE prunes the subtree too.
    check(find_widget(tb, deco) == nullptr, "11 a ROLE_NONE section is dropped");
    check(find_widget(tb, deco_child) == nullptr,
          "11 and its child goes with it");

    // 12  Zero size drops ONLY that node.
    check(find_widget(tb, zero) == nullptr, "12 a 0x0 container is dropped");
    {
      const A11yNode* n = find_widget(tb, zero_child);
      check(n != nullptr, "12 but its visible child SURVIVES");
      if (n) {
        const A11yNode* fbn = find_widget(tb, fb);
        check(fbn && neui_detail::a11y_id_equal(n->parent, fbn->id),
              "12 and re-parents onto the nearest survivor");
      }
    }

    // 4b  A scrolling SECTION is a scroll area, not a plain group.
    if (const A11yNode* n = find_widget(tb, scr))
      check_eq_int(n->role, NEUI_A11Y_ROLE_SCROLL_AREA,
                   "4  scrolling SECTION -> SCROLL_AREA");

    // 8  OFFSCREEN rather than dropped.
    {
      const A11yNode* first = find_widget(tb, scr_first);
      const A11yNode* last  = find_widget(tb, scr_last);
      check(first && last, "8  both first and last scrolled children are nodes");
      if (first && last) {
        check(!(first->state & NEUI_A11Y_STATE_OFFSCREEN),
              "8  the visible one is not OFFSCREEN");
        check(last->state & NEUI_A11Y_STATE_OFFSCREEN,
              "8  the scrolled-away one IS OFFSCREEN (present, not pruned)");
      }
    }

    // 13  TABVIEW chips + hidden page pruned.
    {
      const A11yNode* n = find_widget(tb, tv);
      check(n != nullptr, "13 the tabview is a node");
      if (n) check_eq_int(n->role, NEUI_A11Y_ROLE_TAB_LIST, "13 TABVIEW -> TAB_LIST");
      check_eq_int(count_sub(tb, tv, A11ySubKind::tab_chip), 2,
                   "13 one TAB per page");
      const A11yNode* c0 = find_sub(tb, tv, A11ySubKind::tab_chip, 0);
      const A11yNode* c1 = find_sub(tb, tv, A11ySubKind::tab_chip, 1);
      if (c0) {
        check_eq_int(c0->role, NEUI_A11Y_ROLE_TAB, "13 chip role is TAB");
        check_eq_str(c0->name, "One", "13 chip takes its page's label");
        check(c0->state & NEUI_A11Y_STATE_SELECTED, "13 the active chip is SELECTED");
      }
      if (c1)
        check(!(c1->state & NEUI_A11Y_STATE_SELECTED),
              "13 the inactive chip is not");
      check(find_widget(tb, p1b) != nullptr,
            "13 the visible page's content is present");
      check(find_widget(tb, p2b) == nullptr,
            "13 the hidden page's content is pruned");
    }

    // 14  COMBOBOX closed vs open.
    {
      const A11yNode* n = find_widget(tb, combo);
      check(n != nullptr, "14 the combobox is a node");
      if (n) {
        check_eq_int(n->role, NEUI_A11Y_ROLE_COMBOBOX, "14 COMBOBOX -> COMBOBOX");
        check(n->state & NEUI_A11Y_STATE_COLLAPSED,
              "14 a closed combobox reports COLLAPSED");
        check_eq_int(n->total_child_count, 3, "14 and still reports its 3 items");
      }
      check_eq_int(count_sub(tb, combo, A11ySubKind::list_row), 0,
                   "14 a closed combobox emits no rows (they are not on screen)");

      // Open it by clicking the collapsed bar, then rebuild.
      click_in(wb, 12.0f + 75.0f, 192.0f + 11.0f);
      pump(0.15);
      auto tb2 = xpl_host::a11y_build_tree_for_frame(fb);
      const A11yNode* n2 = find_widget(tb2, combo);
      check(n2 && (n2->state & NEUI_A11Y_STATE_EXPANDED),
            "14 an open combobox reports EXPANDED");
      check_eq_int(count_sub(tb2, combo, A11ySubKind::list_row), 3,
                   "14 and its rows appear");
      const A11yNode* r0 = find_sub(tb2, combo, A11ySubKind::list_row, 0);
      if (r0 && n2) {
        check_eq_str(r0->name, "alpha", "14 open row 0 carries its item text");
        // The overlay is drawn BELOW the collapsed bar (there is room), so the
        // rows must not be reported on top of the bar itself.
        check(r0->y >= n2->y + n2->h,
              "14 open rows sit on the overlay, not on the collapsed bar");
      }
      // Dismiss it. An open combo consumes EVERY click in the frame
      // (handle_combo_click), so leaving it open would make the next check's
      // click land on this overlay instead of its own target.
      post_key(wb, 0x35);                 // kVK_Escape
      pump(0.10);
    }

    // 17  An overlay that escapes a clipped ancestor is NOT offscreen.
    std::printf("\n-- overlay vs ancestor clip --\n");
    {
      const A11yNode* sn = find_widget(tb, scr2);
      const A11yNode* cn = find_widget(tb, combo2);
      check(sn && cn, "17 the clipped section and its combobox are nodes");
      if (sn && cn) {
        // Open it by clicking its collapsed bar, at the rect the adapter reports.
        click_in(wb, (float)(cn->x + cn->w / 2), (float)(cn->y + cn->h / 2));
        pump(0.15);
        auto t3 = xpl_host::a11y_build_tree_for_frame(fb);
        const int rows = count_sub(t3, combo2, A11ySubKind::list_row);
        check(rows > 0, "17 the drop rows appear");
        // 6 rows x 18px of list from a 44px-tall section near the frame's bottom
        // edge: rows land outside the section body in one direction or the other
        // (overlay_rect flips the list ABOVE the bar when below would overflow
        // the frame, which is the case here - so check both directions rather
        // than assuming which).
        int offscreen = 0, outside_section = 0;
        for (const auto& nd : t3) {
          if (nd.id.widget_id != combo2.id) continue;
          if (nd.id.sub_kind != (int32_t)A11ySubKind::list_row) continue;
          if (nd.state & NEUI_A11Y_STATE_OFFSCREEN) ++offscreen;
          if (nd.y + nd.h <= sn->y || nd.y >= sn->y + sn->h) ++outside_section;
        }
        check(outside_section > 0,
              "17 some rows really do fall outside the section body");
        check_eq_int(offscreen, 0,
                     "17 yet no drop row is reported OFFSCREEN");
      }
    }

    // 16  TREEVIEW hierarchy under a SCROLLED window.
    std::printf("\n-- treeview --\n");
    {
      // Expand the group and walk the selection down so the window ends up
      // starting below the depth-0 row. There is no public expand call, so RIGHT
      // does it - and RIGHT needs a selection first (on_keydown bails when
      // nothing is selected), which set_selected provides.
      w->set_focus(sess, tree);
      tree_api->set_selected(sess, tree, troot);
      pump(0.05);
      post_key(wb, 0x7C);                 // kVK_RightArrow - expand
      for (int i = 0; i < 9; ++i) post_key(wb, 0x7D);   // kVK_DownArrow
      pump(0.15);

      auto tt = xpl_host::a11y_build_tree_for_frame(fb);
      const A11yNode* tn = find_widget(tt, tree);
      check(tn != nullptr, "16 the treeview is a node");
      if (!tn) { std::printf("        (skipping the rest)\n"); }
      else {
        check_eq_int(tn->role, NEUI_A11Y_ROLE_TREE, "16 TREEVIEW -> TREE");
        check_eq_int(tn->total_child_count, 13,
                     "16 reports all 13 visible-order items as its total");
        check(tn->first_child_index > 0,
              "16 the window really is scrolled past the group row");

        const int item_nodes = count_sub(tt, tree, A11ySubKind::tree_item);
        check(item_nodes > 0 && item_nodes < 13,
              "16 only the windowed rows are emitted");

        // THE regression: with the window starting below depth 0, every emitted
        // row's parent item is outside the window. Each row must still land
        // INSIDE the treeview - never as a root of the frame, which is what a
        // parent id no node carries degrades to.
        int roots = 0, under_tree = 0, under_item = 0;
        for (const auto& nd : tt) {
          if (nd.id.widget_id != tree.id) continue;
          if (nd.id.sub_kind != (int32_t)A11ySubKind::tree_item) continue;
          if (neui_detail::a11y_id_null(nd.parent)) { ++roots; continue; }
          if (neui_detail::a11y_id_equal(nd.parent, tn->id)) { ++under_tree; continue; }
          const A11yNode* p = neui_detail::a11y_find(tt, nd.parent);
          if (p && p->id.sub_kind == (int32_t)A11ySubKind::tree_item) ++under_item;
          else ++roots;                  // parent named but not present == orphan
        }
        check_eq_int(roots, 0,
                     "16 no tree item escapes the treeview when scrolled");
        check(under_tree + under_item == item_nodes,
              "16 every emitted item is inside the treeview");
        if (roots)
          std::printf("        %d of %d items became roots of the frame\n",
                      roots, item_nodes);

        // Row rects must not overlap the scrollbar gutter - 13 items in a 4-row
        // box means the scrollbar is showing.
        for (const auto& nd : tt) {
          if (nd.id.widget_id != tree.id) continue;
          if (nd.id.sub_kind != (int32_t)A11ySubKind::tree_item) continue;
          check(nd.w < tn->w,
                "16 a row is narrower than the treeview (scrollbar gutter)");
          break;
        }
      }
    }

    // =====================================================================
    // 9  GENERATION - the slot-reuse hazard.
    std::printf("\n-- generation --\n");
    {
      // Take an id for a throwaway widget, destroy it, then create another so
      // the tree slot is recycled. The AT is holding the OLD id.
      neui_widget_t tmp = w->create(sess, fb, NEUI_W_BUTTON, 12, 300, 60, 20, nullptr);
      w->set_text(sess, tmp, "temp");
      pump(0.05);
      const uint32_t slot = tmp.id & 0xffff;
      auto held = xpl_host::a11y_build_tree_for_frame(fb);
      const A11yNode* n = find_widget(held, tmp);
      check(n != nullptr, "9  the throwaway widget is a node");
      A11yNodeId stale = n ? n->id : A11yNodeId{};

      w->destroy(sess, tmp);
      neui_widget_t reused = w->create(sess, fb, NEUI_W_BUTTON, 12, 300, 60, 20, nullptr);
      w->set_text(sess, reused, "reused");
      pump(0.05);
      check_eq_int((long)(reused.id & 0xffff), (long)slot,
                   "9  the new widget really did take the freed slot");

      auto after = xpl_host::a11y_build_tree_for_frame(fb);
      // The stale id must find nothing, EVEN THOUGH a live widget occupies the
      // slot - answering with the new widget's role and name would be a wrong
      // answer, not a missing one.
      check(neui_detail::a11y_find(after, stale) == nullptr,
            "9  a stale id resolves to NOTHING after slot reuse");
      const A11yNode* rn = find_widget(after, reused);
      check(rn != nullptr, "9  the new occupant IS in the tree");
      if (rn)
        check(rn->id.generation != stale.generation,
              "9  and at a different generation");
    }

    std::printf("\n%s (%d failure%s)\n",
                g_failures ? "A11Y TREE FAILED" : "A11Y TREE OK",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
  }
}
