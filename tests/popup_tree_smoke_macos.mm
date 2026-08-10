// Tree-model popup-menu harness (NEUI_W_POPUPMENU + popup_tree_menu),
// macOS / xpl host.
//
// The whole point of this feature is REUSE - the cascade layout, checkmarks,
// shortcut columns, per-item enabling and validate all come from the menubar
// unchanged. So what needs testing is not the layout maths (the menubar already
// exercises that) but the seams where the popup differs:
//
//   1. LEVEL 0 AT THE ANCHOR - the one branch that differs from a menubar, where
//      a popup opens at the click point instead of under a band item.
//   2. ASYNC DISPATCH        - a leaf pick fires NEUI_EVENT_ITEM_SELECTED on the
//      POPUPMENU carrying the tree item id, and the call itself never blocks.
//   3. CASCADE              - clicking a parent opens its submenu rather than
//      reporting a pick.
//   4. NON-PICKS            - separators, disabled items ("section headers") and
//      a click outside must report NOTHING, and only the outside click dismisses.
//   5. REJECTIONS           - an empty menu, a non-POPUPMENU widget, and a
//      MENUBAR must all be refused rather than opening something useless.
//
// Clicks are delivered as real NSEvents into the view's -mouseDown:, i.e. the
// same entry point AppKit uses, so the production path runs end to end.
//
// Needs a GUI session, so it is built but not ctest-registered; run
// ./tests/<config>/neui_popup_tree_smoke_macos manually.

#import <AppKit/AppKit.h>

#include <neui/neui.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int           g_failures = 0;
NSWindow*     g_window   = nil;
neui_widget_t g_menu{};

struct Pick { bool seen = false; uint32_t item = 0; int count = 0; };
Pick g_pick;

void check(bool ok, const char* what)
{
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) ++g_failures;
}

void check_eq(uint32_t got, uint32_t want, const char* what)
{
  bool ok = (got == want);
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) { std::printf("        got %u, want %u\n", got, want); ++g_failures; }
}

bool NEUI_ABI onevent(void*, neui_event_t* ev)
{
  if (ev->type == NEUI_EVENT_ITEM_SELECTED &&
      ev->data.item.widget.id == g_menu.id) {
    g_pick.seen = true;
    g_pick.item = ev->data.item.index;
    ++g_pick.count;
    return true;
  }
  return false;
}

neui_widget_client_t g_widget_client = { NEUI_VERSION, nullptr, onevent };

void* NEUI_ABI get_interface(void*, const char* iface)
{
  if (iface && std::strcmp(iface, NEUI_API_WIDGETS) == 0) return &g_widget_client;
  return nullptr;
}

// Click at a FRAME-local logical point (y down from the client top-left).
void click_at(float lx, float ly)
{
  NSView* v = [g_window contentView];
  if (!v) return;
  const CGFloat h = [v bounds].size.height;
  NSEvent* ev = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                    location:NSMakePoint(lx, h - ly)
                               modifierFlags:0
                                   timestamp:0
                                windowNumber:[g_window windowNumber]
                                     context:nil
                                 eventNumber:0
                                  clickCount:1
                                    pressure:1];
  if (ev) [v mouseDown:ev];
}

} // namespace

int main()
{
  @autoreleasepool {
    neui_init();

    neui_api_t* neui = neui_get_api("neui.host.crossplatform");
    if (!neui) { std::printf("[FAIL] xpl host not registered\n"); return 1; }

    neui_client_t client = { NEUI_VERSION, get_interface };
    neui_session_t sess  = neui->create_session(&client, nullptr);

    auto* widgets = (neui_widget_api_t*) neui->get_interface(sess, NEUI_API_WIDGETS);
    auto* tree    = (neui_tree_api_t*)   neui->get_interface(sess, NEUI_API_TREE);
    if (!widgets || !tree) { std::printf("[FAIL] missing interfaces\n"); return 1; }
    if (!widgets->popup_tree_menu) {
      std::printf("[FAIL] popup_tree_menu is null on the xpl host\n");
      return 1;
    }

    neui_widget_t win = widgets->create(sess, widget_none, NEUI_W_APPWINDOW,
                                          100, 100, 480, 400, nullptr);
    neui_widget_t pad = widgets->create(sess, win, NEUI_W_CUSTOMDRAW,
                                          40, 30, 380, 320, nullptr);
    g_menu = widgets->create(sess, win, NEUI_W_POPUPMENU, 0, 0, 0, 0, nullptr);

    // A realistic menu: a header (disabled), two leaves, a separator, and a
    // submenu with a leaf inside it.
    neui_item_t hdr   = tree->add(sess, g_menu, tree_item_root, "Section",  nullptr);
    neui_item_t copy  = tree->add(sess, g_menu, tree_item_root, "Copy",     nullptr);
    neui_item_t paste = tree->add(sess, g_menu, tree_item_root, "Paste",    nullptr);
    /* sep */           tree->add(sess, g_menu, tree_item_root, "-",        nullptr);
    neui_item_t more   = tree->add(sess, g_menu, tree_item_root, "More",    nullptr);
    /* nested */         tree->add(sess, g_menu, more,           "Deeper",  nullptr);

    tree->set_shortcut(sess, g_menu, copy, NEUI_KMOD_CTRL, NEUI_KEY_C);
    tree->set_enabled(sess, g_menu, hdr, false);      // "section header"

    widgets->show(sess, win);
    neui->pump_once(sess);

    g_window = [[NSApp windows] count] ? [[NSApp windows] objectAtIndex:0] : nil;
    if (!g_window) { std::printf("[FAIL] no NSWindow realized\n"); return 1; }

    // ---- 5. rejections --------------------------------------------------
    {
      neui_widget_t empty = widgets->create(sess, win, NEUI_W_POPUPMENU, 0,0,0,0, nullptr);
      check(!widgets->popup_tree_menu(sess, pad, 10, 10, empty),
              "an empty POPUPMENU is refused rather than opening a blank box");
      neui_widget_t mb = widgets->create(sess, win, NEUI_W_MENUBAR, 0,0,0,0, nullptr);
      tree->add(sess, mb, tree_item_root, "File", nullptr);
      check(!widgets->popup_tree_menu(sess, pad, 10, 10, mb),
              "a MENUBAR is refused (it is not a POPUPMENU)");
      check(!widgets->popup_tree_menu(sess, pad, 10, 10, pad),
              "a non-menu widget is refused");
      widgets->destroy(sess, mb);
      widgets->destroy(sess, empty);
    }

    // ---- 1 + 2. opens at the anchor, and a leaf pick reports async -------
    // The anchor is the pad at frame (40, 30); (x, y) are pad-local, so a popup
    // at pad-local (20, 10) opens at frame (60, 40). Row 0 is the header, row 1
    // is "Copy" - one POPUP_ITEM_H below it. Item height is 22 and the column
    // pads by 4, so row 1's middle sits ~4 + 22 + 11 = 37 px below the origin.
    const float ox = 60.0f, oy = 40.0f;
    g_pick = Pick{};
    check(widgets->popup_tree_menu(sess, pad, 20, 10, g_menu),
            "popup_tree_menu opens");
    check(!g_pick.seen, "opening fires nothing on its own (it is async, not blocking)");

    click_at(ox + 30.0f, oy + 37.0f);          // "Copy"
    check(g_pick.seen, "clicking a leaf reports a pick");
    check_eq(g_pick.item, copy.id, "the pick carries the tree item id");
    check_eq((uint32_t)g_pick.count, 1u, "exactly one event per pick");

    // ---- 4. non-picks ---------------------------------------------------
    // Disabled header: row 0, ~4 + 11 = 15 px down. Must not report, and must
    // not dismiss either (an OS menu stays open on a dead click).
    g_pick = Pick{};
    widgets->popup_tree_menu(sess, pad, 20, 10, g_menu);
    click_at(ox + 30.0f, oy + 15.0f);
    check(!g_pick.seen, "a disabled item (section header) reports nothing");

    // Click far outside: dismiss, still no pick.
    click_at(400.0f, 380.0f);
    check(!g_pick.seen, "a click outside reports nothing");
    g_pick = Pick{};
    click_at(ox + 30.0f, oy + 37.0f);
    check(!g_pick.seen,
            "after dismissal the popup is gone (that click hit no menu)");

    // ---- 3. cascade -----------------------------------------------------
    // Geometry is DISCOVERED, not assumed. Column widths are content-derived
    // from font metrics, so hardcoding them makes the test brittle - and worse,
    // a miss would masquerade as "reported nothing", which is exactly the shape
    // of vacuous check this suite has been bitten by before. So: a menu whose
    // only row is the submenu parent (so row 0's centre, already proven above by
    // the "Copy" pick, is the target), then scan rightwards for the submenu
    // column and require that a pick lands on the nested item.
    {
      neui_widget_t m2 = widgets->create(sess, win, NEUI_W_POPUPMENU, 0,0,0,0, nullptr);
      neui_item_t   parent = tree->add(sess, m2, tree_item_root, "More",   nullptr);
      neui_item_t   nested = tree->add(sess, m2, parent,         "Deeper", nullptr);
      (void)parent;

      // Re-point the event filter at this menu.
      const neui_widget_t saved = g_menu;
      g_menu = m2;

      const float row0_cy = oy + 15.0f;   // pad + half an item, as proven above
      g_pick = Pick{};
      check(widgets->popup_tree_menu(sess, pad, 20, 10, m2), "reopen for cascade");
      click_at(ox + 30.0f, row0_cy);
      check(!g_pick.seen, "clicking a submenu parent reports no pick");

      // Scan BOTH axes: the submenu column starts at the parent column's right
      // edge (content-derived width) and is aligned with the parent ROW, so its
      // first row sits slightly above the parent row's centre, not below it.
      bool nested_hit = false;
      for (float dx = 40.0f; dx < 420.0f && !nested_hit; dx += 8.0f) {
        for (float dy = 6.0f; dy <= 34.0f && !nested_hit; dy += 4.0f) {
          g_pick = Pick{};
          widgets->popup_tree_menu(sess, pad, 20, 10, m2);
          click_at(ox + 30.0f, row0_cy);            // open the submenu
          click_at(ox + dx, oy + dy);               // try the submenu's row 0
          if (g_pick.seen && g_pick.item == nested.id) nested_hit = true;
        }
      }
      check(nested_hit, "a pick inside the opened submenu reports the nested item");

      g_menu = saved;
      widgets->destroy(sess, m2);
    }

    (void)paste;
    neui->destroy(sess);

    if (g_failures) std::printf("\nPOPUP TREE FAILED (%d)\n", g_failures);
    else            std::printf("\nPOPUP TREE OK\n");
    return g_failures ? 1 : 0;
  }
}
