// Option / Command in mouse events, and the modifier keys as keys (macOS, both
// hosts).
//
// What this exists to pin down, against REAL NSEvents going through the real
// -mouseDown: / -flagsChanged: of each host's view rather than against the
// portable translation tests/test_mouse_modifiers.cpp already covers:
//
//   1. MOUSE  - a click / release / move / right-click carries NEUI_MK_ALT when
//               Option is down and NEUI_MK_COMMAND when Command is, neither for
//               the other, neither for no modifier, and Control stays Control
//               (it is NOT where Command lands: NEUI_KMOD_CTRL is Cmd, NEUI_MK_
//               CONTROL is the Control key).
//   2. KEYS   - a modifier pressed or released on its own reaches the focused
//               widget as NEUI_EVENT_KEYDOWN / _KEYUP with NEUI_KEY_SHIFT /
//               _CONTROL / _ALT / _COMMAND. AppKit reports it only as
//               flagsChanged:, with the new flags and no record of the old, so
//               this is what proves the previous-flags diff works end to end.
//   3. STATE  - `modifiers` on those events is the NEUI_KMOD_* state AFTER the
//               change (Cmd is NEUI_KMOD_CTRL, Control is NEUI_KMOD_META), and a
//               flag that did not move produces no event.
//   4. QUIET  - nothing else changes: a modifier produces no KEYCHAR, Caps Lock
//               (which also arrives as flagsChanged:) produces nothing, and on the
//               xpl host a focused INPUTBOX's text is untouched by all of it.
//
// Driven on the crossplatform host (the NEUIView content view) and the native
// host (the NEUINativePaintedView of a CUSTOMDRAW). Realizes real NSWindows, so
// built but NOT ctest-registered; run ./tests/<config>/neui_modifier_keys_smoke_macos
// manually.

#import <AppKit/AppKit.h>

#include <neui/neui.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) ++g_failures;
}

// What the client saw on the widget under test. `a` is the buttonmap of a mouse
// event, or the keycode of a key event; `b` is a key event's modifiers.
struct Rec { neui_event_type_t type; uint32_t a; uint32_t b; };
std::vector<Rec> g_log;
uint32_t         g_target = 0;

bool onevent(void*, neui_event_t* ev)
{
  switch (ev->type) {
    case NEUI_EVENT_MOUSE_MOVE:
    case NEUI_EVENT_MOUSE_BUTTON_DOWN:
    case NEUI_EVENT_MOUSE_BUTTON_UP:
    case NEUI_EVENT_MOUSE_BUTTON_CLICK:
    case NEUI_EVENT_MOUSE_RBUTTON_DOWN:
    case NEUI_EVENT_MOUSE_RBUTTON_UP:
      if (ev->data.mouse.widget.id == g_target)
        g_log.push_back({ ev->type, ev->data.mouse.buttonmap, 0 });
      break;
    case NEUI_EVENT_KEYDOWN:
    case NEUI_EVENT_KEYUP:
    case NEUI_EVENT_KEYCHAR:
      if (ev->data.key.widget.id == g_target)
        g_log.push_back({ ev->type, ev->data.key.keycode, ev->data.key.modifiers });
      break;
    default: break;
  }
  return false;
}

neui_widget_client_t g_wc = { NEUI_VERSION, nullptr, onevent };
void* iface(void*, const char* n)
{ return std::strcmp(n, NEUI_API_WIDGETS) ? nullptr : (void*)&g_wc; }
neui_client_t g_client = { NEUI_VERSION, iface };

NSWindow* window_titled(NSString* title)
{
  for (NSWindow* w in [NSApp windows])
    if ([[w title] isEqualToString:title]) return w;
  return nil;
}

// The native host's CUSTOMDRAW is a NEUINativePaintedView (the class name may
// carry the NEUI_OBJC_SUFFIX, hence the prefix match).
NSView* find_painted(NSView* v)
{
  if ([NSStringFromClass([v class]) hasPrefix:@"NEUINativePaintedView"]) return v;
  for (NSView* s in [v subviews])
    if (NSView* f = find_painted(s)) return f;
  return nil;
}

void pump_briefly()
{
  [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.15]];
}

// A mouse event at a frame-local (top-left origin) point of the window's content
// view, carrying `flags` as its modifier flags.
NSEvent* mouse_event(NSEventType t, NSWindow* win, float lx, float ly,
                     NSEventModifierFlags flags)
{
  const CGFloat h = [[win contentView] bounds].size.height;
  return [NSEvent mouseEventWithType:t
                            location:NSMakePoint(lx, h - ly)
                       modifierFlags:flags
                           timestamp:0
                        windowNumber:[win windowNumber]
                             context:nil
                         eventNumber:0
                          clickCount:1
                            pressure:1.0];
}

NSEvent* flags_event(NSWindow* win, NSEventModifierFlags flags, unsigned short keycode)
{
  return [NSEvent keyEventWithType:NSEventTypeFlagsChanged
                          location:NSZeroPoint
                     modifierFlags:flags
                         timestamp:0
                      windowNumber:[win windowNumber]
                           context:nil
                        characters:@""
       charactersIgnoringModifiers:@""
                         isARepeat:NO
                           keyCode:keycode];
}

// Every NEUI_MK_* a mouse event carries, filtered to the modifier bits so the
// held-button bits (which depend on the real hardware state) cannot interfere.
constexpr uint32_t kModBits = NEUI_MK_SHIFT | NEUI_MK_CONTROL | NEUI_MK_ALT | NEUI_MK_COMMAND;

// Run the whole battery against one host. `view` receives the events (the
// content view on xpl, the CUSTOMDRAW's painted view on native); `px, py` is a
// frame-local point inside the CUSTOMDRAW.
void run_checks(const char* host, NSWindow* win, NSView* view, float px, float py)
{
  std::printf("\n-- %s --\n", host);

  // ---- 1. mouse ----------------------------------------------------------
  struct Case { const char* name; NSEventModifierFlags flags; uint32_t want; };
  const Case cases[] = {
    { "no modifier",     0,                        0 },
    { "Option",          NSEventModifierFlagOption,  NEUI_MK_ALT },
    { "Command",         NSEventModifierFlagCommand, NEUI_MK_COMMAND },
    { "Option+Command",  NSEventModifierFlagOption | NSEventModifierFlagCommand,
                                                   NEUI_MK_ALT | NEUI_MK_COMMAND },
    { "Control",         NSEventModifierFlagControl, NEUI_MK_CONTROL },
    { "Shift+Option",    NSEventModifierFlagShift | NSEventModifierFlagOption,
                                                   NEUI_MK_SHIFT | NEUI_MK_ALT },
  };
  for (const Case& c : cases) {
    char what[128];
    g_log.clear();
    [view mouseDown:mouse_event(NSEventTypeLeftMouseDown, win, px, py, c.flags)];
    std::snprintf(what, sizeof what, "%s: MOUSE_BUTTON_DOWN buttonmap modifiers", c.name);
    bool seen = false;
    for (const Rec& r : g_log)
      if (r.type == NEUI_EVENT_MOUSE_BUTTON_DOWN) { seen = true; check((r.a & kModBits) == c.want, what); }
    if (!seen) check(false, what);

    g_log.clear();
    [view mouseUp:mouse_event(NSEventTypeLeftMouseUp, win, px, py, c.flags)];
    std::snprintf(what, sizeof what, "%s: MOUSE_BUTTON_UP buttonmap modifiers", c.name);
    seen = false;
    for (const Rec& r : g_log)
      if (r.type == NEUI_EVENT_MOUSE_BUTTON_UP) { seen = true; check((r.a & kModBits) == c.want, what); }
    if (!seen) check(false, what);
  }

  g_log.clear();
  [view mouseMoved:mouse_event(NSEventTypeMouseMoved, win, px, py,
                               NSEventModifierFlagOption | NSEventModifierFlagCommand)];
  {
    bool seen = false, ok = true;
    for (const Rec& r : g_log)
      if (r.type == NEUI_EVENT_MOUSE_MOVE) { seen = true; ok = ok && (r.a & kModBits) == (NEUI_MK_ALT | NEUI_MK_COMMAND); }
    check(seen && ok, "Option+Command: MOUSE_MOVE buttonmap modifiers");
  }

  g_log.clear();
  [view rightMouseDown:mouse_event(NSEventTypeRightMouseDown, win, px, py, NSEventModifierFlagOption)];
  {
    bool seen = false, ok = true;
    for (const Rec& r : g_log)
      if (r.type == NEUI_EVENT_MOUSE_RBUTTON_DOWN) { seen = true; ok = ok && (r.a & kModBits) == NEUI_MK_ALT; }
    check(seen && ok, "Option: MOUSE_RBUTTON_DOWN buttonmap modifiers");
  }
  [view mouseUp:mouse_event(NSEventTypeLeftMouseUp, win, px, py, 0)];   // leave nothing pressed

  // ---- 2./3. the modifier keys as keys ------------------------------------
  // Each step is a flagsChanged: with the NEW combined flags, exactly what
  // AppKit sends. kVK_Shift 0x38, kVK_Control 0x3B, kVK_Option 0x3A, kVK_Command 0x37.
  auto step = [&](NSEventModifierFlags flags, unsigned short kc,
                  const char* what, uint32_t want_key, bool want_down, uint32_t want_mods) {
    g_log.clear();
    [view flagsChanged:flags_event(win, flags, kc)];
    char msg[160];
    std::snprintf(msg, sizeof msg, "%s", what);
    const bool one = g_log.size() == 1;
    const bool ok  = one && g_log[0].type == (want_down ? NEUI_EVENT_KEYDOWN : NEUI_EVENT_KEYUP)
                         && g_log[0].a == want_key && g_log[0].b == want_mods;
    check(ok, msg);
    if (!ok) {
      std::printf("        got %zu event(s)", g_log.size());
      for (const Rec& r : g_log)
        std::printf("  [type %#x key %#x mods %#x]", (unsigned)r.type, r.a, r.b);
      std::printf("\n");
    }
  };

  const NSEventModifierFlags S = NSEventModifierFlagShift, C = NSEventModifierFlagControl,
                             O = NSEventModifierFlagOption, M = NSEventModifierFlagCommand;
  step(S,         0x38, "Shift down   -> KEYDOWN NEUI_KEY_SHIFT, KMOD_SHIFT",
       NEUI_KEY_SHIFT, true, NEUI_KMOD_SHIFT);
  step(S | O,     0x3A, "Option down   -> KEYDOWN NEUI_KEY_ALT, KMOD_SHIFT|ALT",
       NEUI_KEY_ALT, true, NEUI_KMOD_SHIFT | NEUI_KMOD_ALT);
  step(S | O | M, 0x37, "Command down  -> KEYDOWN NEUI_KEY_COMMAND, Cmd is KMOD_CTRL",
       NEUI_KEY_COMMAND, true, NEUI_KMOD_SHIFT | NEUI_KMOD_ALT | NEUI_KMOD_CTRL);
  step(S | O | M | C, 0x3B, "Control down  -> KEYDOWN NEUI_KEY_CONTROL, Control is KMOD_META",
       NEUI_KEY_CONTROL, true, NEUI_KMOD_SHIFT | NEUI_KMOD_ALT | NEUI_KMOD_CTRL | NEUI_KMOD_META);
  step(S | O | C, 0x37, "Command up    -> KEYUP NEUI_KEY_COMMAND, KMOD_CTRL cleared",
       NEUI_KEY_COMMAND, false, NEUI_KMOD_SHIFT | NEUI_KMOD_ALT | NEUI_KMOD_META);
  step(S | C,     0x3A, "Option up     -> KEYUP NEUI_KEY_ALT",
       NEUI_KEY_ALT, false, NEUI_KMOD_SHIFT | NEUI_KMOD_META);
  step(C,         0x38, "Shift up      -> KEYUP NEUI_KEY_SHIFT",
       NEUI_KEY_SHIFT, false, NEUI_KMOD_META);
  step(0,         0x3B, "Control up    -> KEYUP NEUI_KEY_CONTROL, no modifiers left",
       NEUI_KEY_CONTROL, false, 0);

  // A flag that did not move is no event: the same flags again, and Caps Lock
  // (which also arrives as flagsChanged:) are both silent.
  g_log.clear();
  [view flagsChanged:flags_event(win, 0, 0x38)];
  check(g_log.empty(), "flagsChanged: with unchanged flags reports nothing");
  g_log.clear();
  [view flagsChanged:flags_event(win, NSEventModifierFlagCapsLock, 0x39)];
  check(g_log.empty(), "Caps Lock down reports nothing");
  [view flagsChanged:flags_event(win, 0, 0x39)];

  // ---- 4. a modifier has no text ------------------------------------------
  g_log.clear();
  [view flagsChanged:flags_event(win, O, 0x3A)];
  [view flagsChanged:flags_event(win, 0, 0x3A)];
  bool any_char = false;
  for (const Rec& r : g_log) if (r.type == NEUI_EVENT_KEYCHAR) any_char = true;
  check(!any_char && g_log.size() == 2, "Option down + up: two key events, no KEYCHAR");
}

} // namespace

int main()
{
  @autoreleasepool {
    [NSApplication sharedApplication];
    neui_init();

    const struct { const char* id; const char* title; bool native; } hosts[] = {
      { "neui.host.crossplatform", "modkeys xpl",    false },
      { "neui.host.macos",         "modkeys native", true  },
    };

    for (const auto& h : hosts) {
      neui_api_t* api = neui_get_api(h.id);
      if (!api) { std::printf("FAIL: no %s host\n", h.id); return 1; }
      void* app = nullptr;
      neui_session_t sess = api->create_session(&g_client, &app);
      auto* w = (neui_widget_api_t*)api->get_interface(sess, NEUI_API_WIDGETS);
      if (!w) { std::printf("FAIL: no widgets interface on %s\n", h.id); return 1; }

      // 320x200 client; the CUSTOMDRAW sits at (20,20) 120x80, so (50,50) is on it.
      neui_widget_t win = w->create(sess, widget_none, NEUI_W_APPWINDOW, 80, 120, 320, 200, nullptr);
      w->set_text(sess, win, h.title);
      neui_widget_t cd = w->create(sess, win, NEUI_W_CUSTOMDRAW, 20, 20, 120, 80, nullptr);
      // The xpl host draws its own text widgets, so an INPUTBOX there is what
      // proves the built-in text input ignores the new keys (check 4). On the
      // native host it is an NSTextField, which never sees these events.
      neui_widget_t edit{};
      if (!h.native) {
        edit = w->create(sess, win, NEUI_W_INPUTBOX, 20, 120, 200, 24, nullptr);
        w->set_text(sess, edit, "abc");
      }
      w->show(sess, win);
      pump_briefly();
      g_target = cd.id;

      NSWindow* nswin = window_titled([NSString stringWithUTF8String:h.title]);
      if (!nswin) { std::printf("FAIL: %s window not realized\n", h.id); return 1; }
      NSView* view = h.native ? find_painted([nswin contentView]) : [nswin contentView];
      if (!view) { std::printf("FAIL: no target view on %s\n", h.id); return 1; }

      // Keyboard focus on the CUSTOMDRAW: the xpl host routes keys to its focused
      // widget; on the native host the painted view is itself the receiver.
      w->set_focus(sess, cd);
      pump_briefly();

      run_checks(h.id, nswin, view, 50.0f, 50.0f);

      if (!h.native) {
        // 4. (cont.) A focused INPUTBOX: the client still gets the key events,
        // but no character is inserted and the text is untouched.
        w->set_focus(sess, edit);
        pump_briefly();
        g_target = edit.id;
        g_log.clear();
        [view flagsChanged:flags_event(nswin, NSEventModifierFlagShift, 0x38)];
        [view flagsChanged:flags_event(nswin, NSEventModifierFlagShift | NSEventModifierFlagCommand, 0x37)];
        [view flagsChanged:flags_event(nswin, 0, 0x37)];
        [view flagsChanged:flags_event(nswin, 0, 0x38)];
        char text[16] = {};
        w->get_text(sess, edit, text, (int)sizeof text);
        check(std::strcmp(text, "abc") == 0, "focused INPUTBOX: text untouched by modifier keys");
        bool down = false, up = false, chr = false;
        for (const Rec& r : g_log) {
          if (r.type == NEUI_EVENT_KEYDOWN && r.a == NEUI_KEY_SHIFT) down = true;
          if (r.type == NEUI_EVENT_KEYUP   && r.a == NEUI_KEY_COMMAND) up = true;
          if (r.type == NEUI_EVENT_KEYCHAR) chr = true;
        }
        check(down && up && !chr && g_log.size() == 4,
              "focused INPUTBOX: client sees the 4 key events, no KEYCHAR");
      }
    }

    std::printf("\n%s\n", g_failures ? "MODIFIER KEYS FAILED" : "modifier keys: all checks passed");
    return g_failures ? 1 : 0;
  }
}
