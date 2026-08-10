// Tree-model context-menu harness (NEUI_W_POPUPMENU + widgets->popup_tree_menu),
// macOS / xpl host.
//
// Every check here exists because the FIRST attempt at this feature shipped the
// bug it now guards. That version made PopupMenuWidget inherit MenubarWidget
// without overriding is_menubar(), so a popup was a menu BAR to every predicate
// in the host - and the harness that "verified" it did not notice, because it
// only ever asserted that picking a row reported the right id. So the checks
// below are deliberately aimed at the SEAMS, not at the happy path:
//
//   A. NOT-A-MENU-BAR   - a POPUPMENU child of a frame must not become the
//                         frame's menu bar. Read from [NSApp mainMenu], the
//                         thing the user actually sees, not from our own flags.
//   B. ROOT ROWS ARE ROWS - on a MENUBAR, root children are top-level titles
//                         (no command id, never selectable, "-" meaningless).
//                         On a POPUPMENU they are the first column of ORDINARY
//                         rows, so set_menu_cmd / set_checked / "-" must all
//                         work there. Every one of those silently no-opped.
//   C. COMMAND SCOPING  - cmd ids restart at 0x8000 per menu widget, so the
//                         popup and the menu bar BOTH own 0x8000. A pick must
//                         route to the popup's item, not the menu bar's.
//   D. NO STUCK GRAB    - destroying the menu (or emptying it) while open must
//                         not leave an invisible modal grab that swallows every
//                         later click in the frame.
//   E. NO LEAK-THROUGH  - a pick, a dismiss, and a double-click must not also
//                         reach the widget under the popup.
//
// Row geometry is DISCOVERED by probing, never hardcoded: probe_pick() reopens
// the menu and clicks one point, so a whole column can be mapped a pixel row at
// a time. Hardcoding content-derived widths is what let the earlier harness
// mistake a miss for "reported nothing".
//
// Needs a GUI session (it realizes a real NSWindow), so it is built but not
// ctest-registered; run ./tests/<config>/neui_popup_tree_smoke_macos manually.

#import <AppKit/AppKit.h>

#include <neui/neui.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int       g_failures = 0;
NSWindow* g_window   = nil;

neui_api_t*        g_neui    = nullptr;
neui_session_t     g_sess    = { 0 };
neui_widget_api_t* g_widgets = nullptr;

// ---- recorded events -------------------------------------------------------
// The client's view of what happened. Every check reads these rather than any
// host-internal state, so the harness cannot pass by agreeing with itself.
struct Recorded {
  uint32_t popup_item        = 0;   // ITEM_SELECTED .index (0 = none seen)
  uint32_t popup_widget      = 0;   // ITEM_SELECTED .widget.id
  int      popup_count       = 0;   // how many ITEM_SELECTED arrived
  uint32_t activated_widget  = 0;   // TREE_ITEM_ACTIVATED .widget.id
  uint32_t activated_item    = 0;
  int      activated_count   = 0;
  int      under_up          = 0;   // MOUSE_BUTTON_UP on the widget beneath
  int      under_click       = 0;   // MOUSE_BUTTON_CLICK   "
  int      under_dbl         = 0;   // MOUSE_BUTTON_DBLCLICK "
  int      under_down        = 0;   // MOUSE_BUTTON_DOWN     "
};
Recorded g_rec;
uint32_t g_under_id = 0;            // the BUTTON that sits beneath the popup

void reset_rec() { g_rec = Recorded(); }

void check(bool ok, const char* what)
{
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) ++g_failures;
}

void check_eq(uint32_t got, uint32_t want, const char* what)
{
  bool ok = (got == want);
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) {
    std::printf("        got %u, want %u\n", got, want);
    ++g_failures;
  }
}

// ---- event injection -------------------------------------------------------
// locationInWindow is y-UP from the window's bottom-left; the view is
// isFlipped:YES and converts back, so the y axis is flipped here. These go
// through the same -mouseDown: / -mouseUp: AppKit uses, so the whole production
// path runs (platform layer -> Session::handle_tree_popup_* -> client).

NSEvent* make_mouse_event(NSEventType type, float lx, float ly, int click_count)
{
  NSView* v = [g_window contentView];
  const CGFloat h = [v bounds].size.height;
  return [NSEvent mouseEventWithType:type
                            location:NSMakePoint(lx, h - ly)
                       modifierFlags:0
                           timestamp:0
                        windowNumber:[g_window windowNumber]
                             context:nil
                         eventNumber:0
                          clickCount:click_count
                            pressure:1.0];
}

// A full press+release pair, as a real click delivers. Both halves matter: the
// release is exactly what leaked to the widget underneath before mouseUp: was
// gated, so a harness that only sent the press could not have seen it.
void click_at(float lx, float ly, int click_count = 1)
{
  NSView* v = [g_window contentView];
  if (!v) return;
  [v mouseDown:make_mouse_event(NSEventTypeLeftMouseDown, lx, ly, click_count)];
  [v mouseUp:make_mouse_event(NSEventTypeLeftMouseUp, lx, ly, click_count)];
}

// A press with NO paired release. Models the gesture that produced the
// stale-swallow defect: on win32 a press consumed by the popup takes no mouse
// capture, so dragging off the window and releasing outside delivers no
// WM_LBUTTONUP at all. macOS/X11 do guarantee paired delivery, so this cannot
// happen there for real - but the SESSION-level flag is shared by all three
// platforms, so the fix is testable here.
void press_only_at(float lx, float ly)
{
  NSView* v = [g_window contentView];
  if (!v) return;
  [v mouseDown:make_mouse_event(NSEventTypeLeftMouseDown, lx, ly, 1)];
}

void move_to(float lx, float ly)
{
  NSView* v = [g_window contentView];
  if (!v) return;
  [v mouseMoved:make_mouse_event(NSEventTypeMouseMoved, lx, ly, 0)];
}

// ---- popup driving ---------------------------------------------------------

const int kAnchorX = 40;   // popup opens at frame-local (kAnchorX, kAnchorY)
const int kAnchorY = 60;

bool open_popup(neui_widget_t frame, neui_widget_t menu)
{
  if (!g_widgets->popup_tree_menu) return false;
  return g_widgets->popup_tree_menu(g_sess, frame, kAnchorX, kAnchorY, menu);
}

// Open the menu fresh and click ONE point in it. Returns the item id reported by
// ITEM_SELECTED, or 0 if the click reported nothing (separator, disabled row,
// submenu parent, or outside). Reopening per probe is what makes a whole column
// mappable without any knowledge of row heights.
uint32_t probe_pick(neui_widget_t frame, neui_widget_t menu, float x, float y)
{
  reset_rec();
  if (!open_popup(frame, menu)) return 0;
  click_at(x, y);
  return g_rec.popup_item;
}

bool NEUI_ABI onevent(void*, neui_event_t* ev)
{
  switch (ev->type) {
    case NEUI_EVENT_ITEM_SELECTED:
      g_rec.popup_item   = ev->data.item.index;
      g_rec.popup_widget = ev->data.item.widget.id;
      ++g_rec.popup_count;
      return true;
    case NEUI_EVENT_TREE_ITEM_ACTIVATED:
      g_rec.activated_widget = ev->data.tree.widget.id;
      g_rec.activated_item   = ev->data.tree.item.id;
      ++g_rec.activated_count;
      return true;
    case NEUI_EVENT_MOUSE_BUTTON_DOWN:
      if (ev->data.mouse.widget.id == g_under_id) ++g_rec.under_down;
      return false;
    case NEUI_EVENT_MOUSE_BUTTON_UP:
      if (ev->data.mouse.widget.id == g_under_id) ++g_rec.under_up;
      return false;
    case NEUI_EVENT_MOUSE_BUTTON_CLICK:
      if (ev->data.mouse.widget.id == g_under_id) ++g_rec.under_click;
      return false;
    case NEUI_EVENT_MOUSE_BUTTON_DBLCLICK:
      if (ev->data.mouse.widget.id == g_under_id) ++g_rec.under_dbl;
      return false;
    default:
      return false;
  }
}

neui_widget_client_t g_widget_client = { NEUI_VERSION, nullptr, onevent };

void* NEUI_ABI get_interface(void*, const char* iface)
{
  if (iface && std::strcmp(iface, NEUI_API_WIDGETS) == 0) return &g_widget_client;
  return nullptr;
}

// Collect the titles currently on the app's main menu. This is the observable
// for check A: if a POPUPMENU is still menu-BAR-ish anywhere, its rows show up
// here (or replace what's here) on show().
std::vector<std::string> main_menu_titles()
{
  std::vector<std::string> out;
  NSMenu* m = [NSApp mainMenu];
  if (!m) return out;
  for (NSMenuItem* it in [m itemArray])
    out.push_back(std::string([[it title] UTF8String] ? [[it title] UTF8String] : ""));
  return out;
}

bool titles_contain(const std::vector<std::string>& v, const char* needle)
{
  for (const auto& s : v) if (s == needle) return true;
  return false;
}

} // namespace

int main()
{
  @autoreleasepool {
    neui_init();

    // The xpl host explicitly: popup_tree_menu is an xpl-host entry point, and
    // on macOS neui_get_api(NULL) hands back the NATIVE host first (same caveat
    // as NEUI_API_TIMER / _POINTER / _EMBED).
    g_neui = neui_get_api("neui.host.crossplatform");
    if (!g_neui) { std::printf("[FAIL] xpl host not registered\n"); return 1; }

    neui_client_t client = { NEUI_VERSION, get_interface };
    g_sess = g_neui->create_session(&client, nullptr);

    g_widgets   = (neui_widget_api_t*) g_neui->get_interface(g_sess, NEUI_API_WIDGETS);
    auto* tree  = (neui_tree_api_t*)   g_neui->get_interface(g_sess, NEUI_API_TREE);
    if (!g_widgets || !tree) { std::printf("[FAIL] missing interfaces\n"); return 1; }
    if (!g_widgets->popup_tree_menu) {
      std::printf("[FAIL] popup_tree_menu slot is null on the xpl host\n");
      return 1;
    }

    neui_widget_t win = g_widgets->create(g_sess, widget_none, NEUI_W_APPWINDOW,
                                          80, 80, 520, 420, nullptr);

    // A REAL menu bar on the same frame. Two jobs: it is what check A asserts
    // survives, and its leaf item takes cmd_id 0x8000 - the same id the popup's
    // first row gets - which is what check C turns into an observable.
    neui_widget_t mb = g_widgets->create(g_sess, win, NEUI_W_MENUBAR, 0, 0, 0, 0, nullptr);
    neui_item_t   mb_file = tree->add(g_sess, mb, tree_item_root, "File", nullptr);
    neui_item_t   mb_save = tree->add(g_sess, mb, mb_file, "Save", nullptr);
    tree->set_menu_cmd(g_sess, mb, mb_save, NEUI_CMD_USER_BASE + 1);

    // The widget the popup paints over. Placed to span the whole popup column so
    // any leak-through has somewhere to land.
    neui_widget_t under = g_widgets->create(g_sess, win, NEUI_W_BUTTON,
                                            20, 40, 260, 200, nullptr);
    g_widgets->set_text(g_sess, under, "under");
    g_under_id = under.id;

    // The popup. Root children are ordinary rows: two picks, a separator between
    // them, a disabled row, and a submenu parent with one child.
    neui_widget_t pm = g_widgets->create(g_sess, win, NEUI_W_POPUPMENU,
                                         0, 0, 0, 0, nullptr);
    neui_item_t r_cut  = tree->add(g_sess, pm, tree_item_root, "Cut", nullptr);
    neui_item_t r_sep  = tree->add(g_sess, pm, tree_item_root, "-", nullptr);
    neui_item_t r_copy = tree->add(g_sess, pm, tree_item_root, "Copy", nullptr);
    neui_item_t r_dis  = tree->add(g_sess, pm, tree_item_root, "Disabled", nullptr);
    neui_item_t r_more = tree->add(g_sess, pm, tree_item_root, "More", nullptr);
    neui_item_t r_nest = tree->add(g_sess, pm, r_more, "Nested", nullptr);
    tree->set_enabled(g_sess, pm, r_dis, false);
    // Bound so that r_cut carries a cmd_id AND a menu_cmd. r_cut is the popup's
    // FIRST item, so its cmd_id is 0x8000 - the same id mb_save got - which is
    // what makes the scoping check below sharp.
    tree->set_menu_cmd(g_sess, pm, r_cut, NEUI_CMD_USER_BASE + 2);

    // A second, single-row popup plus an INPUTBOX, for the built-in-command
    // check: its one root row is bound to NEUI_CMD_PASTE, so picking it must
    // paste into whatever has focus.
    neui_widget_t box = g_widgets->create(g_sess, win, NEUI_W_INPUTBOX,
                                          300, 300, 200, 26, nullptr);
    neui_widget_t pm2 = g_widgets->create(g_sess, win, NEUI_W_POPUPMENU,
                                          0, 0, 0, 0, nullptr);
    neui_item_t paste_row = tree->add(g_sess, pm2, tree_item_root, "Paste", nullptr);
    tree->set_menu_cmd(g_sess, pm2, paste_row, NEUI_CMD_PASTE);
    auto* clip = (neui_clipboard_api_t*) g_neui->get_interface(g_sess, NEUI_API_CLIPBOARD);

    g_widgets->show(g_sess, win);
    g_neui->pump_once(g_sess);

    g_window = [[NSApp windows] count] ? [[NSApp windows] objectAtIndex:0] : nil;
    if (!g_window) { std::printf("[FAIL] no NSWindow realized\n"); return 1; }

    // ---- A. a POPUPMENU is not a menu bar --------------------------------
    // The first version of this feature made [NSApp setMainMenu:] fire for the
    // POPUPMENU at show(), wiping the app's real menu bar. Read the app menu.
    std::vector<std::string> titles = main_menu_titles();
    check(titles_contain(titles, "File"),
          "the real menu bar survives a POPUPMENU sibling (File still on NSApp mainMenu)");
    check(!titles_contain(titles, "Cut") && !titles_contain(titles, "Copy") &&
          !titles_contain(titles, "More"),
          "no POPUPMENU row leaked onto the app's menu bar");
    // Negative probe for the two checks above: if main_menu_titles() came back
    // empty we would be asserting nothing at all, and both would pass vacuously.
    check(!titles.empty(),
          "precondition: NSApp mainMenu is actually populated (checks above are not vacuous)");

    // ---- B. root rows are ordinary, pickable rows -------------------------
    // set_checked on a root row: on a MENUBAR a root child is a submenu parent
    // and set_checked refuses. On a POPUPMENU it must take.
    tree->set_checked(g_sess, pm, r_copy, true);
    check(tree->get_checked(g_sess, pm, r_copy),
          "set_checked takes on a top-level POPUPMENU row");
    // Negative probe: the same call on a MENUBAR root child must still refuse,
    // or the check above would pass for the wrong reason (set_checked having
    // become unconditional).
    tree->set_checked(g_sess, mb, mb_file, true);
    check(!tree->get_checked(g_sess, mb, mb_file),
          "precondition: set_checked still refuses a MENUBAR top-level title");

    // Map the column a pixel row at a time. x is inside the column: it opens at
    // kAnchorX and is at least POPUP_MIN_W (140) wide.
    const float px = (float)kAnchorX + 30.0f;
    std::vector<uint32_t> col;                 // col[i] = id picked at y = kAnchorY + i
    const int kScan = 160;
    for (int i = 0; i < kScan; ++i)
      col.push_back(probe_pick(win, pm, px, (float)kAnchorY + (float)i));

    auto first_y = [&](uint32_t id) -> int {
      for (int i = 0; i < kScan; ++i) if (col[(size_t)i] == id) return i;
      return -1;
    };
    auto count_y = [&](uint32_t id) -> int {
      int n = 0;
      for (int i = 0; i < kScan; ++i) if (col[(size_t)i] == id) ++n;
      return n;
    };

    const int y_cut  = first_y(r_cut.id);
    const int y_copy = first_y(r_copy.id);
    check(y_cut >= 0,  "the first top-level row (\"Cut\") is pickable");
    check(y_copy > y_cut, "the row after the separator (\"Copy\") is pickable, below Cut");
    check(count_y(r_cut.id) >= 10 && count_y(r_copy.id) >= 10,
          "each pickable row spans a full row height, not a stray pixel");

    // The separator: never pickable, and SHORT. If "-" were still being added as
    // an ordinary row (the shipped bug) it would report r_sep on ~22 px of the
    // column. This is the check the earlier harness omitted - it added a
    // separator and never clicked it.
    check(count_y(r_sep.id) == 0,
          "the \"-\" separator is never reported as a pick");
    int gap = 0;
    for (int i = y_cut; i < y_copy && i >= 0; ++i)
      if (col[(size_t)i] == 0) ++gap;
    check(gap > 0 && gap < 15,
          "the band between Cut and Copy is separator-height (short), not a full row");

    // A disabled row consumes without reporting; the submenu parent descends
    // instead of picking. Both are "reported nothing", so they are only
    // meaningful together with the positive picks above.
    check(count_y(r_dis.id) == 0,  "a disabled row reports no pick");
    check(count_y(r_more.id) == 0, "a submenu parent reports no pick (it descends)");

    // The nested child is reachable: hover the submenu parent to cascade, then
    // click in the second column. Rows are uniform height, so the pitch is
    // Cut's measured span and "More" is two rows past "Copy" (Copy, Disabled,
    // More). The submenu column's position is DISCOVERED by scanning both axes -
    // hardcoding it from the text width is what let the earlier harness mistake
    // a miss for "reported nothing".
    const int pitch  = count_y(r_cut.id);
    const int y_more = y_copy + 2 * pitch;
    check(y_more < kScan && col[(size_t)y_more] == 0,
          "precondition: the row two pitches past Copy is the submenu parent");
    bool nested_ok = false;
    for (int dx = 0; dx < 320 && !nested_ok; dx += 6) {
      for (int dy = -pitch; dy <= 3 * pitch && !nested_ok; dy += 4) {
        // Re-open + re-hover per probe so the cascade state is identical each
        // time (a click either picks or dismisses, so state cannot be reused).
        reset_rec();
        if (!open_popup(win, pm)) continue;
        move_to(px, (float)kAnchorY + (float)y_more);
        click_at((float)kAnchorX + 100.0f + (float)dx,
                 (float)kAnchorY + (float)y_more + (float)dy);
        if (g_rec.popup_item == r_nest.id) nested_ok = true;
      }
    }
    check(nested_ok,
          "hovering a submenu parent opens a cascade whose child is pickable");

    // ---- C. command routing is scoped to the popup ------------------------
    // r_cut and mb_save BOTH hold cmd_id 0x8000: next_menu_cmd_id restarts at
    // 0x8000 per menu widget. The first version routed a popup pick through
    // dispatch_menu_event, which scans Session::_menubars for the id - so this
    // click fired the menu BAR's "Save" item instead. TREE_ITEM_ACTIVATED is the
    // MENUBAR's pick event and must not appear for a popup pick at all.
    reset_rec();
    open_popup(win, pm);
    click_at(px, (float)kAnchorY + (float)y_cut);
    check_eq(g_rec.popup_count, 1u,
             "a command-bound popup row reports exactly one ITEM_SELECTED");
    check_eq(g_rec.popup_widget, pm.id, "ITEM_SELECTED names the POPUPMENU");
    check_eq(g_rec.popup_item, r_cut.id, "ITEM_SELECTED carries the popup's item id");
    check_eq(g_rec.activated_count, 0u,
             "a popup pick fires no TREE_ITEM_ACTIVATED (the menu bar's event)");
    check_eq(g_rec.activated_widget, 0u,
             "no menu-bar item was activated by the popup's colliding cmd id");

    // An unbound row behaves identically: one event, no command.
    reset_rec();
    open_popup(win, pm);
    click_at(px, (float)kAnchorY + (float)y_copy);
    check_eq(g_rec.popup_item, r_copy.id, "an unbound row reports ITEM_SELECTED");
    check_eq(g_rec.activated_count, 0u, "an unbound row routes no command");

    // set_menu_cmd on a top-level popup row actually WORKS - the third thing
    // that silently no-opped there. Verified end-to-end: put text on the
    // clipboard, focus an empty INPUTBOX, pick a row bound to NEUI_CMD_PASTE,
    // and read the box back. Nothing but a real built-in command reaching the
    // focused widget produces that text.
    if (clip && paste_row.id != 0) {
      clip->set_text(g_sess, "pasted-by-popup");
      g_widgets->set_text(g_sess, box, "");
      g_widgets->set_focus(g_sess, box);
      reset_rec();
      open_popup(win, pm2);
      click_at((float)kAnchorX + 30.0f, (float)kAnchorY + (float)(pitch / 2));
      char buf[64] = {0};
      g_widgets->get_text(g_sess, box, buf, (int)sizeof(buf));
      check(std::strcmp(buf, "pasted-by-popup") == 0,
            "a built-in command bound to a top-level popup row reaches the focused widget");
      check_eq(g_rec.popup_count, 1u,
               "a built-in-command row still reports ITEM_SELECTED exactly once");
      // Negative probe: the same pick with focus elsewhere must NOT paste, or
      // the check above could be passing on something other than command routing.
      clip->set_text(g_sess, "second-paste");
      g_widgets->set_text(g_sess, box, "");
      g_widgets->set_focus(g_sess, under);
      open_popup(win, pm2);
      click_at((float)kAnchorX + 30.0f, (float)kAnchorY + (float)(pitch / 2));
      char buf2[64] = {0};
      g_widgets->get_text(g_sess, box, buf2, (int)sizeof(buf2));
      check(std::strcmp(buf2, "second-paste") != 0,
            "precondition: the paste depends on focus (not an unconditional write)");
    } else {
      std::printf("[SKIP] clipboard interface unavailable; built-in-command check skipped\n");
    }

    // ---- E. nothing leaks through to the widget underneath ----------------
    // The BUTTON spans the popup's column, so a leaked press / release / dblclick
    // lands on it. Before mouseUp: was gated, the pick above also delivered an UP.
    reset_rec();
    open_popup(win, pm);
    click_at(px, (float)kAnchorY + (float)y_cut);
    check(g_rec.under_down == 0 && g_rec.under_up == 0 && g_rec.under_click == 0,
          "picking a row sends no DOWN / UP / CLICK to the widget beneath");
    // Negative probe: the same coordinates with NO popup open must reach the
    // button - otherwise "nothing leaked" would be true simply because the
    // button is not hit-testable there.
    reset_rec();
    click_at(px, (float)kAnchorY + (float)y_cut);
    check(g_rec.under_down == 1 && g_rec.under_up == 1,
          "precondition: without a popup those coordinates DO hit the button");

    // Dismissing by clicking outside the column is also consumed.
    reset_rec();
    open_popup(win, pm);
    click_at((float)kAnchorX + 30.0f, (float)kAnchorY - 20.0f);
    check(g_rec.popup_count == 0, "clicking outside the popup reports no pick");
    check(g_rec.under_down == 0 && g_rec.under_up == 0,
          "the dismissing click is consumed, not passed to the widget beneath");
    // ...and the popup really is gone afterwards: the next click works normally.
    reset_rec();
    click_at(px, (float)kAnchorY + (float)y_cut);
    check(g_rec.under_down == 1,
          "after an outside-click dismiss, input reaches widgets again");

    // The armed release-swallow must not outlive its own gesture. Consume a
    // press inside the popup and never deliver its release (see press_only_at);
    // the NEXT, unrelated click must be complete. Before the press-time clear,
    // that click got a DOWN with no UP and no CLICK, left _pressed_widget stuck,
    // and on a KNOB / SLIDER would have left a GESTURE_BEGIN with no
    // GESTURE_END - a stuck beginEdit for a DAW automation client.
    reset_rec();
    open_popup(win, pm);
    press_only_at(px, (float)kAnchorY + (float)y_cut);
    check(g_rec.popup_count == 1, "precondition: the unpaired press was consumed by the popup");
    reset_rec();
    click_at(px, (float)kAnchorY + (float)y_cut);
    check(g_rec.under_down == 1 && g_rec.under_up == 1 && g_rec.under_click == 1,
          "an armed release-swallow does not eat a later, unrelated click's UP");

    // A double-click inside the popup must pick, not bypass the menu. AppKit
    // sends clickCount == 2 on the second press, which -mouseDown: used to
    // convert to DBLCLICK before it ever consulted the popup.
    reset_rec();
    open_popup(win, pm);
    click_at(px, (float)kAnchorY + (float)y_copy, 2);
    check_eq(g_rec.popup_item, r_copy.id,
             "a double-click inside the popup picks the row it lands on");
    check(g_rec.under_dbl == 0,
          "a double-click inside the popup sends no DBLCLICK to the widget beneath");

    // ---- D. no stuck grab -------------------------------------------------
    // These assert the OUTCOME (input still reaches widgets), and three
    // independent mechanisms produce it: widget_destroy calls
    // close_tree_popup_if_within, tp_claim self-heals when the cascade can no
    // longer be built, and every platform hook honours the handler's return
    // value instead of swallowing unconditionally. Verified by probe: removing
    // any ONE still passes; removing all three fails. So no single mechanism is
    // pinned down here - only that a destroyed / emptied menu cannot wedge the
    // frame. (close_tree_popup_if_within additionally drops the stale PAINT,
    // which the client cannot observe from here.)
    //
    // (i) The menu is emptied while open. tree->clear is the documented way to
    // rebuild a menu, so this is reachable from ordinary use.
    reset_rec();
    open_popup(win, pm);
    tree->clear(g_sess, pm);
    click_at(px, (float)kAnchorY + (float)y_cut);
    check(g_rec.under_down == 1,
          "emptying the menu while open does not leave a grab swallowing clicks");

    // Rebuild it and confirm the popup still works after the clear - i.e. clear
    // reset the model rather than wedging it.
    neui_item_t r2 = tree->add(g_sess, pm, tree_item_root, "AfterClear", nullptr);
    reset_rec();
    check(open_popup(win, pm), "a cleared-then-rebuilt POPUPMENU opens again");
    click_at(px, (float)kAnchorY + (float)y_cut);
    check_eq(g_rec.popup_item, r2.id, "the rebuilt row is pickable");

    // (ii) An EMPTY menu must refuse to open rather than put up a box that eats
    // the next click.
    tree->clear(g_sess, pm);
    reset_rec();
    check(!open_popup(win, pm), "an empty POPUPMENU refuses to open");
    click_at(px, (float)kAnchorY + (float)y_cut);
    check(g_rec.under_down == 1,
          "a refused open leaves no grab behind");

    // (iii) The menu widget is DESTROYED while open. This is the DAW-closes-the-
    // editor shape: without close_tree_popup_if_within the frame keeps an
    // invisible modal grab for the rest of its life.
    tree->add(g_sess, pm, tree_item_root, "Doomed", nullptr);
    reset_rec();
    check(open_popup(win, pm), "precondition: the popup is open before destroy");
    g_widgets->destroy(g_sess, pm);
    click_at(px, (float)kAnchorY + (float)y_cut);
    check(g_rec.under_down == 1,
          "destroying the POPUPMENU while open does not leave a stuck grab");

    // The real menu bar is STILL intact after all of that - the popup never
    // touched it, including through clear + destroy (which rebuild / release a
    // MENUBAR's native root).
    titles = main_menu_titles();
    check(titles_contain(titles, "File"),
          "the app menu bar is still intact after the popup's clear + destroy");

    g_neui->pump_once(g_sess);
    g_neui->destroy(g_sess);

    std::printf("\n%s (%d failure%s)\n",
                g_failures == 0 ? "POPUP OK" : "POPUP FAILED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
  }
}
