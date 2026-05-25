// Native macOS host - neui_widget_api_t / neui_items_api_t / neui_tree_api_t /
// neui_attr_api_t / neui_clipboard_api_t / neui_commands_api_t.
//
// Step 3 of plans/native-macos-host.md: wires widget_create / widget_destroy
// (tree slot allocation) and routes widget_show through Session. NSWindow
// plumbing for APPWINDOW lives in window.mm; later steps extend
// widget_show's per-type switch (LABEL / BUTTON / INPUTBOX / …).
//
// Shape mirror of hosts/win32/widgets.cpp.

#import <AppKit/AppKit.h>

#include "host.h"
#include "checkbox_image.h"
#include "../shared/macos/clipboard_macos.h"
#include "../shared/macos/menubar_macos.h"
#include "../shared/compound.h"
#include "../../backends/cg/cg_backend.h"

#include <algorithm>
#include <cstring>

// Defined in window.mm - singleton sink for NSMenuItem activations.
@interface NEUINativeMenuTarget : NSObject
+ (instancetype)shared;
- (void)neuiNativeMenuPick:(id)sender;
@end

namespace macos_host
{
  // Widget id layout: upper 16 = owning session id, lower 16 = tree slot
  // (mirror of hosts/win32/widgets.cpp).

  static uint32_t WidgetToIndex(neui_widget_t widget)
  {
    return widget.id & 0xffff;
  }

  static neui_widget_t IndexToWidget(uint32_t session_id, uint32_t idx)
  {
    return { ((session_id & 0xffff) << 16) | (idx & 0xffff) };
  }

  static Session* get_session(neui_session_t session)
  {
    uint32_t idx = (session.session & 0xffff) - 1;
    if (idx < sessions.size()) return sessions[idx].get();
    return nullptr;
  }

  static bool widget_belongs_to_session(neui_widget_t widget, uint32_t session_id)
  {
    if (widget.id == 0)          return true;   // widget_root
    if (widget.id == UINT32_MAX) return true;   // widget_none
    return ((widget.id >> 16) & 0xffff) == (session_id & 0xffff);
  }

  static Session* get_session_for_widget(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session(session);
    if (!s) return nullptr;
    if (!widget_belongs_to_session(widget, s->session_id())) return nullptr;
    return s;
  }

  // -------------------------------------------------------------------------
  // Session::widget_create / widget_destroy - pure C++ tree manipulation.
  // widget_show lives in window.mm because it touches AppKit.

  neui_widget_t Session::widget_create(neui_widget_t parent, const char* type,
                                        int x, int y, int width, int height,
                                        void* userdata)
  {
    auto wd = std::make_unique<WidgetData>();
    wd->type        = type;
    wd->x           = x;
    wd->y           = y;
    wd->width       = width;
    wd->height      = height;
    wd->visible     = true;
    wd->userdata    = userdata;
    wd->session     = this;
    wd->session_id  = _session_id;
    // Same auto-emit-events list as the win32 / xpl hosts.
    wd->emit_events = type && (
      !strcmp(type, NEUI_W_BUTTON)   ||
      !strcmp(type, NEUI_W_INPUTBOX) ||
      !strcmp(type, NEUI_W_CHECKBOX) ||
      !strcmp(type, NEUI_W_CHECKBOX3)||
      !strcmp(type, NEUI_W_LISTBOX)  ||
      !strcmp(type, NEUI_W_COMBOBOX) ||
      !strcmp(type, NEUI_W_MULTILINE)||
      !strcmp(type, NEUI_W_TREEVIEW) ||
      !strcmp(type, NEUI_W_SLIDER)   ||
      !strcmp(type, NEUI_W_KNOB)     ||
      !strcmp(type, NEUI_W_CUSTOMDRAW));

    // Implicit type variants: CHECKBOX3 = CHECKBOX + tristate=1; MULTILINE
    // = INPUTBOX + multiline=1. Same shape as win32 / xpl.
    if (type && !strcmp(type, NEUI_W_CHECKBOX3))
      neui_detail::ensure_attrs(wd->attrs).set_int(NEUI_ATTR_TRISTATE, 1);
    if (type && !strcmp(type, NEUI_W_MULTILINE))
      neui_detail::ensure_attrs(wd->attrs).set_int(NEUI_ATTR_MULTILINE, 1);

    uint32_t parent_idx = (parent.id == widget_none.id) ? 0 : WidgetToIndex(parent);
    bool     is_root    = (parent.id == widget_none.id);
    if (is_root) wd->isroot = true;

    uint32_t slot = _widgets.add_child(parent_idx, std::move(wd));
    _widgets[slot].index     = slot;
    _widgets[slot].widget_id = IndexToWidget(_session_id, slot).id;

    // MENUBAR: allocate the NSMenu immediately so subsequent t_add /
    // t_set_shortcut calls have a target, even before widget_show. The
    // Cmd+Q App menu is prepended once; setMainMenu: happens on
    // widget_show of the parent frame.
    if (type && !strcmp(type, NEUI_W_MENUBAR)) {
      NSMenu* m = [[NSMenu alloc] init];
      [m setAutoenablesItems:NO];
      neui_detail::macos_install_app_menu(m);
      _widgets[slot].native_control = (__bridge_retained void*)m;
    }

    return IndexToWidget(_session_id, slot);
  }

  // Forward declared in window.mm - releases the +1-retained NSWindow /
  // NSControl held in wd.native_window / wd.native_control via __bridge_transfer.
  void release_native_window_macos (WidgetData& wd);
  void release_native_control_macos(WidgetData& wd);
  // Defined in window.mm. Sends setNeedsDisplay:YES to the widget's
  // NSView when it has one (used by the compound-attached invalidation
  // hooks and widget_invalidate).
  void mark_widget_dirty_for_paint (WidgetData& wd);

  void Session::widget_destroy(neui_widget_t widget)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return;

    // Depth-first child destroy so every descendant's ondestroy fires before
    // the parent's. Same shape as the win32 host.
    {
      uint32_t child = _widgets.child(index);
      while (child != 0) {
        uint32_t next = _widgets.next(child);
        if (_widgets.exists(child))
          widget_destroy({ _widgets[child].widget_id });
        child = next;
      }
    }

    auto& w = _widgets[index];
    if (_client_widget_api && _client_widget_api->ondestroy)
      _client_widget_api->ondestroy(_token, widget, w.userdata);

    if (w.native_control) release_native_control_macos(w);
    if (w.native_window)  release_native_window_macos (w);

    _widgets.remove(index);
  }

  // -------------------------------------------------------------------------
  // neui_widget_api_t

  static neui_widget_t NEUI_ABI w_create(neui_session_t session, neui_widget_t parent,
                                          const char* type, int x, int y,
                                          int w, int h, void* udata)
  {
    auto* s = get_session_for_widget(session, parent);
    if (!s) return widget_none;
    return s->widget_create(parent, type, x, y, w, h, udata);
  }

  static void NEUI_ABI w_destroy(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (s) s->widget_destroy(widget);
  }

  static void NEUI_ABI w_show(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (s) s->widget_show(widget);
  }

  static void NEUI_ABI w_hide(neui_session_t, neui_widget_t)                    {}
  static void NEUI_ABI w_set_pos(neui_session_t, neui_widget_t,
                                  int, int, int, int)                          {}
  static void NEUI_ABI w_set_size(neui_session_t, neui_widget_t, int, int)      {}
  static void NEUI_ABI w_set_emit_events(neui_session_t session, neui_widget_t widget, bool enabled)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (s->_widgets.exists(i)) s->_widgets[i].emit_events = enabled;
  }
  // Live-sync helper: round-trip a widget's text through its NSControl when
  // one exists. Used by w_set_text / w_get_text so the example's
  // get_text(input) reads what the user actually typed instead of the stale
  // wd.text snapshot from create-time.
  static NSString* widget_native_string(WidgetData& wd)
  {
    if (!wd.native_control) return nil;
    NSView* v = (__bridge NSView*)wd.native_control;
    if ([v isKindOfClass:[NSTextField class]])
      return ((NSTextField*)v).stringValue;
    if ([v isKindOfClass:[NSScrollView class]]) {
      NSView* doc = ((NSScrollView*)v).documentView;
      if ([doc isKindOfClass:[NSTextView class]])
        return ((NSTextView*)doc).string;
    }
    if ([v isKindOfClass:[NSButton class]])
      return ((NSButton*)v).title;
    return nil;
  }

  static void widget_set_native_string(WidgetData& wd, NSString* s)
  {
    if (!wd.native_control || !s) return;
    NSView* v = (__bridge NSView*)wd.native_control;
    if ([v isKindOfClass:[NSTextField class]]) {
      ((NSTextField*)v).stringValue = s;
      return;
    }
    if ([v isKindOfClass:[NSScrollView class]]) {
      NSView* doc = ((NSScrollView*)v).documentView;
      if ([doc isKindOfClass:[NSTextView class]])
        ((NSTextView*)doc).string = s;
      return;
    }
    if ([v isKindOfClass:[NSButton class]]) {
      ((NSButton*)v).title = s;
      return;
    }
  }

  // Defined in window.mm. For NEUINativePaintedView instances, drop the
  // cached image bitmap so the next drawRect: reloads from the (new) text
  // path. No-op for non-painted views.
  void reset_image_bitmap_cache_macos(WidgetData& wd);

  static void NEUI_ABI w_set_text(neui_session_t session, neui_widget_t widget, const char* text)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    wd.text = text ? text : "";
    NSString* ns = [NSString stringWithUTF8String:wd.text.c_str()];
    if (wd.native_window) {
      [(__bridge NSWindow*)wd.native_window setTitle:ns];
    } else if (wd.native_control) {
      widget_set_native_string(wd, ns);
      // IMAGE: source path drives the painted view's cached CGImage.
      // Drop the cache so the next paint reloads. SECTION also re-paints
      // (title chip is text-driven).
      if (wd.type && (!strcmp(wd.type, NEUI_W_IMAGE) ||
                       !strcmp(wd.type, NEUI_W_SECTION))) {
        if (!strcmp(wd.type, NEUI_W_IMAGE)) reset_image_bitmap_cache_macos(wd);
        mark_widget_dirty_for_paint(wd);
      }
    }
  }
  static int  NEUI_ABI w_get_text(neui_session_t session, neui_widget_t widget, char* buf, int buflen)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return 0;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return 0;
    auto& wd = s->_widgets[i];

    // Pull live text out of the native control (NSTextField / NSTextView /
    // NSButton) before falling back to the cached wd.text. Mirrors win32's
    // GetWindowTextW pattern.
    NSString* live = widget_native_string(wd);
    std::string cached;
    const char* src = wd.text.c_str();
    if (live) {
      cached = live.UTF8String ? live.UTF8String : "";
      src    = cached.c_str();
    }
    int len = (int)strlen(src);
    int needed = len + 1;
    if (buf && buflen > 0) {
      int n = (buflen < needed) ? buflen : needed;
      if (n > 0) {
        memcpy(buf, src, (size_t)(n - 1));
        buf[n - 1] = '\0';
      }
    }
    return needed;
  }
  static neui_widget_t NEUI_ABI w_get_first_child (neui_session_t, neui_widget_t) { return widget_none; }
  static neui_widget_t NEUI_ABI w_get_next_sibling(neui_session_t, neui_widget_t) { return widget_none; }
  static void NEUI_ABI w_set_focus(neui_session_t, neui_widget_t)               {}
  static void NEUI_ABI w_set_check(neui_session_t session, neui_widget_t widget,
                                    neui_check_state_t state)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    // Cache the logical state - the click handler in window.mm reads this
    // to advance the 3-state cycle correctly across calls.
    neui_detail::ensure_attrs(wd.attrs).set_int("neui.macoshost.checkstate",
                                                  (int32_t)state);
    if (!wd.native_control) return;
    NSView* v = (__bridge NSView*)wd.native_control;
    if (![v isKindOfClass:[NSButton class]]) return;
    ((NSButton*)v).image = checkbox_image_for_state((int)state);
  }
  static neui_check_state_t NEUI_ABI w_get_check(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return NEUI_CHECK_UNCHECKED;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return NEUI_CHECK_UNCHECKED;
    auto& wd = s->_widgets[i];
    // Prefer the cached logical state when present - it's the source of
    // truth for the 3-state cycle. Fall back to the NSButton state for
    // 2-state checkboxes whose state was driven entirely by AppKit clicks.
    if (wd.attrs && wd.attrs->has("neui.macoshost.checkstate")) {
      int32_t v = wd.attrs->get_int("neui.macoshost.checkstate",
                                      NEUI_CHECK_UNCHECKED);
      return (neui_check_state_t)v;
    }
    if (!wd.native_control) return NEUI_CHECK_UNCHECKED;
    NSView* v = (__bridge NSView*)wd.native_control;
    if (![v isKindOfClass:[NSButton class]]) return NEUI_CHECK_UNCHECKED;
    NSControlStateValue ns_state = ((NSButton*)v).state;
    if (ns_state == NSControlStateValueOn)    return NEUI_CHECK_CHECKED;
    if (ns_state == NSControlStateValueMixed) return NEUI_CHECK_INDETERMINATE;
    return NEUI_CHECK_UNCHECKED;
  }
  static void* NEUI_ABI w_get_native_handle(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return nullptr;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return nullptr;
    auto& wd = s->_widgets[i];
    return wd.native_window ? wd.native_window : wd.native_control;
  }
  static void  NEUI_ABI w_set_tab_stop(neui_session_t, neui_widget_t, bool)     {}
  static void  NEUI_ABI w_set_owner(neui_session_t session, neui_widget_t dialog, neui_widget_t owner)
  {
    auto* s = get_session_for_widget(session, dialog);
    if (!s) return;
    uint32_t i = WidgetToIndex(dialog);
    if (s->_widgets.exists(i))
      s->_widgets[i].owner_index = (owner.id == widget_none.id) ? 0 : WidgetToIndex(owner);
  }
  static void  NEUI_ABI w_get_pos (neui_session_t session, neui_widget_t widget, int* x, int* y)
  {
    auto* s = get_session_for_widget(session, widget);
    int gx = 0, gy = 0;
    if (s) {
      uint32_t i = WidgetToIndex(widget);
      if (s->_widgets.exists(i)) { gx = s->_widgets[i].x; gy = s->_widgets[i].y; }
    }
    if (x) *x = gx; if (y) *y = gy;
  }
  static void  NEUI_ABI w_get_size(neui_session_t session, neui_widget_t widget, int* w, int* h)
  {
    auto* s = get_session_for_widget(session, widget);
    int gw = 0, gh = 0;
    if (s) {
      uint32_t i = WidgetToIndex(widget);
      if (s->_widgets.exists(i)) { gw = s->_widgets[i].width; gh = s->_widgets[i].height; }
    }
    if (w) *w = gw; if (h) *h = gh;
  }
  static int   NEUI_ABI w_popup_menu(neui_session_t, neui_widget_t, int, int,
                                      const char* const*)
  {
    return 0;
  }

  // Request a repaint of the widget. For painted views (IMAGE, KNOB,
  // CUSTOMDRAW) this is setNeedsDisplay:YES which coalesces with any
  // other pending paints until the next runloop drain. For native
  // NSControl-backed widgets (NSButton / NSTextField / ...) AppKit
  // owns their invalidation, so the helper is a no-op for them.
  static void  NEUI_ABI w_invalidate(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    mark_widget_dirty_for_paint(s->_widgets[i]);
  }

  // Bind an asset handle to a widget. v1 only supports CUSTOMDRAW +
  // compound: attaching a NEUI_ASSET_KIND_COMPOUND to a CUSTOMDRAW
  // widget switches its paint path from imperative WIDGET_PAINT dispatch
  // to declarative compound-layer walk. asset_none clears the binding.
  //
  // NEUI_W_IMAGE on the native macOS host still loads via
  // [ensureImageBitmap:] from set_text (path-source only) - migrating
  // IMAGE to use asset handles too is a follow-on, see
  // plans/macos-customdraw-and-compound.md.
  static void NEUI_ABI w_set_asset(neui_session_t session,
                                     neui_widget_t widget, neui_asset_t asset)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    if (!wd.type) return;

    // Cross-session handles silently dropped (asset_none is the documented
    // clear sentinel and always passes).
    if (asset.id != asset_none.id &&
        ((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) {
      return;
    }

    if (!strcmp(wd.type, NEUI_W_CUSTOMDRAW)) {
      wd.compound_asset = asset;
      mark_widget_dirty_for_paint(wd);
    }
    // Other widget types: no-op until the native IMAGE path picks this up.
  }

  neui_widget_api_t widgets_api = {
    w_create, w_destroy, w_show, w_hide,
    w_set_pos, w_set_size, w_set_emit_events,
    w_set_text, w_get_text,
    w_get_first_child, w_get_next_sibling,
    w_set_focus,
    w_set_check, w_get_check,
    w_get_native_handle,
    w_set_tab_stop,
    w_set_owner,
    w_get_pos, w_get_size,
    w_popup_menu,
    w_invalidate,
    w_set_asset,
  };

  // -------------------------------------------------------------------------
  // Remaining APIs are no-op scaffolds until their respective step lands.

  // Reload the NSTableView for a LISTBOX or rebuild an NSPopUpButton's menu
  // for a COMBOBOX. No-op when native_control is null - items added before
  // widget_show are synced into the NSPopUpButton inside create_combobox
  // (see window.mm) and the NSTableView pulls from wd.items lazily via
  // its dataSource.
  static void reload_native_items(WidgetData& wd)
  {
    if (!wd.native_control) return;
    NSView* v = (__bridge NSView*)wd.native_control;
    if ([v isKindOfClass:[NSScrollView class]]) {
      NSView* doc = ((NSScrollView*)v).documentView;
      if ([doc isKindOfClass:[NSTableView class]])
        [(NSTableView*)doc reloadData];
      return;
    }
    if ([v isKindOfClass:[NSPopUpButton class]]) {
      NSPopUpButton* pb = (NSPopUpButton*)v;
      [pb removeAllItems];
      for (auto& it : wd.items) {
        // -addItemWithTitle: dedupes by title; preserve duplicates by adding
        // empty items first then setting the title.
        [pb addItemWithTitle:@""];
        [pb.lastItem setTitle:[NSString stringWithUTF8String:it.text.c_str()]];
      }
      if (wd.selected_item != NEUI_ITEM_NONE
          && wd.selected_item < wd.items.size())
        [pb selectItemAtIndex:(NSInteger)wd.selected_item];
    }
  }

  static void NEUI_ABI i_clear(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    s->_widgets[i].items.clear();
    s->_widgets[i].selected_item = NEUI_ITEM_NONE;
    reload_native_items(s->_widgets[i]);
  }

  static uint32_t NEUI_ABI i_add(neui_session_t session, neui_widget_t widget,
                                  const char* text, void* userdata)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return NEUI_ITEM_NONE;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return NEUI_ITEM_NONE;
    auto& wd = s->_widgets[i];
    WidgetData::ItemEntry e;
    e.text     = text ? text : "";
    e.userdata = userdata;
    wd.items.push_back(std::move(e));
    reload_native_items(wd);
    return (uint32_t)(wd.items.size() - 1);
  }

  static void NEUI_ABI i_remove(neui_session_t session, neui_widget_t widget, uint32_t idx)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    if (idx >= wd.items.size()) return;
    wd.items.erase(wd.items.begin() + (ptrdiff_t)idx);
    if (wd.selected_item != NEUI_ITEM_NONE) {
      if (wd.selected_item == idx) wd.selected_item = NEUI_ITEM_NONE;
      else if (wd.selected_item > idx) --wd.selected_item;
    }
    reload_native_items(wd);
  }

  static uint32_t NEUI_ABI i_count(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return 0;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return 0;
    return (uint32_t)s->_widgets[i].items.size();
  }

  static int NEUI_ABI i_get_text(neui_session_t session, neui_widget_t widget,
                                  uint32_t idx, char* buf, int buflen)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return 0;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return 0;
    auto& wd = s->_widgets[i];
    if (idx >= wd.items.size()) return 0;
    const auto& t = wd.items[idx].text;
    int needed = (int)t.size() + 1;
    if (buf && buflen > 0) {
      int n = (buflen < needed) ? buflen : needed;
      memcpy(buf, t.c_str(), (size_t)(n - 1));
      buf[n - 1] = '\0';
    }
    return needed;
  }

  static void NEUI_ABI i_set_text(neui_session_t session, neui_widget_t widget,
                                   uint32_t idx, const char* text)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    if (idx >= wd.items.size()) return;
    wd.items[idx].text = text ? text : "";
    reload_native_items(wd);
  }

  static void* NEUI_ABI i_get_userdata(neui_session_t session, neui_widget_t widget,
                                        uint32_t idx)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return nullptr;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return nullptr;
    auto& wd = s->_widgets[i];
    if (idx >= wd.items.size()) return nullptr;
    return wd.items[idx].userdata;
  }

  static uint32_t NEUI_ABI i_get_selected(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return NEUI_ITEM_NONE;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return NEUI_ITEM_NONE;
    return s->_widgets[i].selected_item;
  }

  static void NEUI_ABI i_set_selected(neui_session_t session, neui_widget_t widget, uint32_t idx)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    if (idx != NEUI_ITEM_NONE && idx >= wd.items.size()) return;
    wd.selected_item = idx;
    if (wd.native_control) {
      NSView* v = (__bridge NSView*)wd.native_control;
      if ([v isKindOfClass:[NSScrollView class]]) {
        NSView* doc = ((NSScrollView*)v).documentView;
        if ([doc isKindOfClass:[NSTableView class]]) {
          NSTableView* tv = (NSTableView*)doc;
          if (idx == NEUI_ITEM_NONE) {
            [tv deselectAll:nil];
          } else {
            [tv selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)idx]
            byExtendingSelection:NO];
            [tv scrollRowToVisible:(NSInteger)idx];
          }
        }
      } else if ([v isKindOfClass:[NSPopUpButton class]]) {
        NSPopUpButton* pb = (NSPopUpButton*)v;
        if (idx == NEUI_ITEM_NONE) [pb selectItemAtIndex:-1];
        else                       [pb selectItemAtIndex:(NSInteger)idx];
      }
    }
  }

  neui_items_api_t items_api = {
    i_clear, i_add, i_remove, i_count,
    i_get_text, i_set_text, i_get_userdata,
    i_get_selected, i_set_selected,
  };

  // Reload the NSOutlineView when the tree contents change. Called from
  // every t_* mutation below.
  static void reload_native_tree(WidgetData& wd)
  {
    if (!wd.native_control) return;
    NSView* v = (__bridge NSView*)wd.native_control;
    if ([v isKindOfClass:[NSScrollView class]]) {
      NSView* doc = ((NSScrollView*)v).documentView;
      if ([doc isKindOfClass:[NSOutlineView class]])
        [(NSOutlineView*)doc reloadData];
    }
  }

  // MENUBAR helpers - walk an NSMenu hierarchy by tag; build NSMenu structure
  // from neui tree calls.

  static NSMenuItem* find_menu_item_by_tag(NSMenu* root, uint32_t target_tag)
  {
    if (!root) return nil;
    for (NSMenuItem* it in root.itemArray) {
      if ((uint32_t)it.tag == target_tag) return it;
      if (it.submenu) {
        NSMenuItem* found = find_menu_item_by_tag(it.submenu, target_tag);
        if (found) return found;
      }
    }
    return nil;
  }

  static bool widget_is_menubar(WidgetData& wd)
  {
    return wd.type && !strcmp(wd.type, NEUI_W_MENUBAR);
  }

  static neui_item_t NEUI_ABI t_add(neui_session_t session, neui_widget_t widget,
                                     neui_item_t parent, const char* text, void* userdata)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return tree_item_none;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return tree_item_none;
    auto& wd = s->_widgets[i];

    if (widget_is_menubar(wd)) {
      if (!wd.native_control) return tree_item_none;
      NSMenu* root = (__bridge NSMenu*)wd.native_control;
      uint32_t id = wd.next_tree_id++;
      NSString* title = neui_detail::macos_menu_title_only(text ? text : "");

      // Mirror the item into wd.tree_items so t_get_text / t_get_userdata
      // work uniformly across MENUBAR + TREEVIEW. Without this the
      // example's `ud == (void*)20` check on the Help → About activation
      // never fires (get_userdata returns nullptr).
      WidgetData::TreeNode node;
      node.parent_id = (parent.id == tree_item_root.id) ? 0 : parent.id;
      node.text      = text ? text : "";
      node.userdata  = userdata;
      wd.tree_items[id] = std::move(node);
      wd.tree_items_ordered.push_back(id);

      // Separator: the public neui convention is text == "-".
      if (text && !strcmp(text, "-")) {
        NSMenuItem* sep = [NSMenuItem separatorItem];
        sep.tag = (NSInteger)id;
        // Find target menu (parent's submenu, or root if top-level).
        NSMenu* target = root;
        if (parent.id != tree_item_root.id) {
          NSMenuItem* p = find_menu_item_by_tag(root, parent.id);
          if (p && p.submenu) target = p.submenu;
        }
        [target addItem:sep];
        return { id };
      }

      if (parent.id == tree_item_root.id) {
        // Top-level popup: NSMenuItem holding a submenu.
        NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
        NSMenu* sub = [[NSMenu alloc] initWithTitle:title];
        [sub setAutoenablesItems:NO];
        it.submenu = sub;
        it.tag = (NSInteger)id;
        [root addItem:it];
        return { id };
      }

      // Nested item: leaf with action, OR submenu if user later adds a child
      // under it (we model as both - start as leaf, promote to submenu on
      // first child add). For v1 simplicity, assume two-level menubars
      // (top → leaves) - the example fits that. Three-level can layer in
      // later by detecting an existing leaf-with-tag and converting.
      NSMenuItem* p = find_menu_item_by_tag(root, parent.id);
      if (!p) return tree_item_none;
      if (!p.submenu) {
        // Promote leaf to popup. Drop its action since it's now a container.
        NSMenu* sub = [[NSMenu alloc] initWithTitle:p.title];
        [sub setAutoenablesItems:NO];
        p.submenu = sub;
        p.action  = nil;
      }
      NSMenuItem* leaf = [[NSMenuItem alloc] initWithTitle:title
                                                     action:@selector(neuiNativeMenuPick:)
                                              keyEquivalent:@""];
      leaf.target             = [NEUINativeMenuTarget shared];
      leaf.tag                = (NSInteger)id;
      leaf.representedObject  = @(wd.widget_id);
      leaf.enabled            = YES;
      [p.submenu addItem:leaf];
      return { id };
    }

    // TREEVIEW path.
    uint32_t id = wd.next_tree_id++;
    WidgetData::TreeNode n;
    n.parent_id = (parent.id == tree_item_root.id) ? 0 : parent.id;
    n.text      = text ? text : "";
    n.userdata  = userdata;
    wd.tree_items[id] = std::move(n);
    wd.tree_items_ordered.push_back(id);
    reload_native_tree(wd);
    return { id };
  }

  static void NEUI_ABI t_remove(neui_session_t session, neui_widget_t widget, neui_item_t item)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    // Recursive removal: children first.
    std::vector<uint32_t> to_remove;
    to_remove.push_back(item.id);
    for (size_t k = 0; k < to_remove.size(); ++k) {
      uint32_t pid = to_remove[k];
      for (uint32_t cid : wd.tree_items_ordered) {
        auto it = wd.tree_items.find(cid);
        if (it != wd.tree_items.end() && it->second.parent_id == pid)
          to_remove.push_back(cid);
      }
    }
    for (uint32_t id : to_remove) wd.tree_items.erase(id);
    wd.tree_items_ordered.erase(
      std::remove_if(wd.tree_items_ordered.begin(), wd.tree_items_ordered.end(),
        [&](uint32_t id) { return wd.tree_items.find(id) == wd.tree_items.end(); }),
      wd.tree_items_ordered.end());
    reload_native_tree(wd);
  }

  static void NEUI_ABI t_clear(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    wd.tree_items.clear();
    wd.tree_items_ordered.clear();
    wd.selected_tree_item = UINT32_MAX;
    reload_native_tree(wd);
  }

  static int NEUI_ABI t_get_text(neui_session_t session, neui_widget_t widget,
                                  neui_item_t item, char* buf, int buflen)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return 0;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return 0;
    auto& wd = s->_widgets[i];
    auto it = wd.tree_items.find(item.id);
    if (it == wd.tree_items.end()) return 0;
    const auto& t = it->second.text;
    int needed = (int)t.size() + 1;
    if (buf && buflen > 0) {
      int n = (buflen < needed) ? buflen : needed;
      memcpy(buf, t.c_str(), (size_t)(n - 1));
      buf[n - 1] = '\0';
    }
    return needed;
  }

  static void NEUI_ABI t_set_text(neui_session_t session, neui_widget_t widget,
                                   neui_item_t item, const char* text)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    auto it = wd.tree_items.find(item.id);
    if (it == wd.tree_items.end()) return;
    it->second.text = text ? text : "";
    reload_native_tree(wd);
  }

  static void* NEUI_ABI t_get_userdata(neui_session_t session, neui_widget_t widget,
                                        neui_item_t item)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return nullptr;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return nullptr;
    auto& wd = s->_widgets[i];
    auto it = wd.tree_items.find(item.id);
    return (it != wd.tree_items.end()) ? it->second.userdata : nullptr;
  }

  static void NEUI_ABI t_set_enabled(neui_session_t session, neui_widget_t widget,
                                      neui_item_t item, bool enabled)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto it = s->_widgets[i].tree_items.find(item.id);
    if (it != s->_widgets[i].tree_items.end()) it->second.enabled = enabled;
  }

  static bool NEUI_ABI t_get_enabled(neui_session_t session, neui_widget_t widget,
                                      neui_item_t item)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return true;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return true;
    auto it = s->_widgets[i].tree_items.find(item.id);
    return (it != s->_widgets[i].tree_items.end()) ? it->second.enabled : true;
  }

  static void NEUI_ABI t_set_shortcut(neui_session_t session, neui_widget_t widget,
                                       neui_item_t item,
                                       uint32_t modifiers, uint32_t key)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    if (!widget_is_menubar(wd) || !wd.native_control) return;
    NSMenu* root = (__bridge NSMenu*)wd.native_control;
    NSMenuItem* it = find_menu_item_by_tag(root, item.id);
    if (!it) return;
    if (key == NEUI_KEY_NONE) {
      it.keyEquivalent             = @"";
      it.keyEquivalentModifierMask = 0;
      return;
    }
    it.keyEquivalent             = neui_detail::macos_key_to_keyEquivalent(key);
    it.keyEquivalentModifierMask = neui_detail::macos_neui_mods_to_appkit(modifiers);
  }

  static neui_item_t NEUI_ABI t_get_first_child(neui_session_t session, neui_widget_t widget,
                                                  neui_item_t parent)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return tree_item_none;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return tree_item_none;
    auto& wd = s->_widgets[i];
    uint32_t pid = (parent.id == tree_item_root.id) ? 0 : parent.id;
    for (uint32_t id : wd.tree_items_ordered) {
      auto it = wd.tree_items.find(id);
      if (it != wd.tree_items.end() && it->second.parent_id == pid) return { id };
    }
    return tree_item_none;
  }

  static neui_item_t NEUI_ABI t_get_next_sibling(neui_session_t session, neui_widget_t widget,
                                                  neui_item_t item)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return tree_item_none;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return tree_item_none;
    auto& wd = s->_widgets[i];
    auto it = wd.tree_items.find(item.id);
    if (it == wd.tree_items.end()) return tree_item_none;
    uint32_t pid = it->second.parent_id;
    bool seen = false;
    for (uint32_t id : wd.tree_items_ordered) {
      if (seen) {
        auto cit = wd.tree_items.find(id);
        if (cit != wd.tree_items.end() && cit->second.parent_id == pid) return { id };
      }
      if (id == item.id) seen = true;
    }
    return tree_item_none;
  }

  static neui_item_t NEUI_ABI t_get_selected(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return tree_item_none;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return tree_item_none;
    uint32_t sel = s->_widgets[i].selected_tree_item;
    return (sel == UINT32_MAX) ? tree_item_none : neui_item_t{ sel };
  }

  static void NEUI_ABI t_set_selected(neui_session_t session, neui_widget_t widget, neui_item_t item)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    uint32_t target = (item.id == tree_item_none.id) ? UINT32_MAX : item.id;
    wd.selected_tree_item = target;
    if (!wd.native_control) return;
    NSView* v = (__bridge NSView*)wd.native_control;
    if (![v isKindOfClass:[NSScrollView class]]) return;
    NSView* doc = ((NSScrollView*)v).documentView;
    if (![doc isKindOfClass:[NSOutlineView class]]) return;
    NSOutlineView* ov = (NSOutlineView*)doc;
    if (target == UINT32_MAX) { [ov deselectAll:nil]; return; }
    NSInteger row = [ov rowForItem:@(target)];
    if (row < 0) return;
    [ov selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
       byExtendingSelection:NO];
    [ov scrollRowToVisible:row];
  }

  static void NEUI_ABI t_set_menu_cmd(neui_session_t, neui_widget_t,
                                       neui_item_t, uint32_t)                   {}

  neui_tree_api_t tree_api = {
    t_add, t_remove, t_clear,
    t_get_text, t_set_text, t_get_userdata,
    t_set_enabled, t_get_enabled,
    t_set_shortcut,
    t_get_first_child, t_get_next_sibling,
    t_get_selected, t_set_selected,
    t_set_menu_cmd,
  };

  // Attribute API: minimal real impl so frame attrs (icon path, min/max,
  // theme mode) can be stored before the per-step polish lands. No live
  // re-application yet - that comes with step 14.

  static int     NEUI_ABI a_set_int   (neui_session_t session, neui_widget_t widget,
                                        const char* key, int32_t value)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return 0;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return 0;
    auto& wd = s->_widgets[i];
    neui_detail::ensure_attrs(wd.attrs).set_int(key, value);
    // CUSTOMDRAW + compound: any attr touch can drive a layer binding or
    // template substitution, so request a repaint. Idempotent + cheap
    // (AppKit coalesces setNeedsDisplay).
    if (wd.type && !strcmp(wd.type, NEUI_W_CUSTOMDRAW) &&
        wd.compound_asset.id != asset_none.id) {
      mark_widget_dirty_for_paint(wd);
    }
    // SECTION: NEUI_ATTR_BACKGROUND drives the body fill colour.
    if (wd.type && !strcmp(wd.type, NEUI_W_SECTION) &&
        !strcmp(key, NEUI_ATTR_BACKGROUND)) {
      mark_widget_dirty_for_paint(wd);
    }
    return 1;
  }
  static int32_t NEUI_ABI a_get_int   (neui_session_t session, neui_widget_t widget,
                                        const char* key, int32_t def)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return def;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i) || !s->_widgets[i].attrs) return def;
    return s->_widgets[i].attrs->get_int(key, def);
  }
  static int     NEUI_ABI a_set_string(neui_session_t session, neui_widget_t widget,
                                        const char* key, const char* value)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return 0;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return 0;
    auto& wd = s->_widgets[i];
    neui_detail::ensure_attrs(wd.attrs).set_string(key, value);
    // CUSTOMDRAW + compound: text-template resolution can change on any
    // string attr touch.
    if (wd.type && !strcmp(wd.type, NEUI_W_CUSTOMDRAW) &&
        wd.compound_asset.id != asset_none.id) {
      mark_widget_dirty_for_paint(wd);
    }
    // SECTION: NEUI_ATTR_ALIGN_TEXT drives the title chip position - the
    // shared paint_section helper reads it each draw, so a repaint is
    // enough to live-update. NEUI_ATTR_BACKGROUND is handled in a_set_int.
    if (wd.type && !strcmp(wd.type, NEUI_W_SECTION) &&
        !strcmp(key, NEUI_ATTR_ALIGN_TEXT)) {
      mark_widget_dirty_for_paint(wd);
    }
    return 1;
  }
  static const char* NEUI_ABI a_get_string(neui_session_t session, neui_widget_t widget,
                                            const char* key)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return nullptr;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i) || !s->_widgets[i].attrs) return nullptr;
    return s->_widgets[i].attrs->get_string(key);
  }
  static int     NEUI_ABI a_has   (neui_session_t session, neui_widget_t widget, const char* key)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return 0;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i) || !s->_widgets[i].attrs) return 0;
    return s->_widgets[i].attrs->has(key) ? 1 : 0;
  }
  static int     NEUI_ABI a_remove(neui_session_t session, neui_widget_t widget, const char* key)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return 0;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i) || !s->_widgets[i].attrs) return 0;
    return s->_widgets[i].attrs->remove(key) ? 1 : 0;
  }
  static int     NEUI_ABI a_set_float(neui_session_t session, neui_widget_t widget,
                                       const char* key, float value)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return 0;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return 0;
    auto& wd = s->_widgets[i];
    neui_detail::ensure_attrs(wd.attrs).set_float(key, value);
    // Live-update SLIDER's NSSlider when NEUI_PARAM_VALUE is the key.
    if (key && !strcmp(key, NEUI_PARAM_VALUE) && wd.native_control) {
      NSView* v = (__bridge NSView*)wd.native_control;
      if ([v isKindOfClass:[NSSlider class]]) {
        float clamped = value;
        if (clamped < 0) clamped = 0; if (clamped > 1) clamped = 1;
        ((NSSlider*)v).doubleValue = clamped;
      }
    }
    // Live-update IMAGE rotation: NEUINativePaintedView reads
    // NEUI_ATTR_ROTATION inside drawRect:, so request a repaint when it
    // changes - analogous to the win32 host's InvalidateRect path.
    if (key && !strcmp(key, NEUI_ATTR_ROTATION) && wd.native_control) {
      NSView* v = (__bridge NSView*)wd.native_control;
      [v setNeedsDisplay:YES];
    }
    // CUSTOMDRAW + compound: any float attr change can drive a binding,
    // so repaint.
    if (wd.type && !strcmp(wd.type, NEUI_W_CUSTOMDRAW) &&
        wd.compound_asset.id != asset_none.id) {
      mark_widget_dirty_for_paint(wd);
    }
    return 1;
  }
  static float   NEUI_ABI a_get_float(neui_session_t session, neui_widget_t widget,
                                       const char* key, float def)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return def;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i) || !s->_widgets[i].attrs) return def;
    return s->_widgets[i].attrs->get_float(key, def);
  }
  static int     NEUI_ABI a_set_session_int(neui_session_t, const char*, int32_t)        { return 0; }
  static int32_t NEUI_ABI a_get_session_int(neui_session_t, const char*, int32_t d)      { return d; }

  neui_attr_api_t attrs_api = {
    NEUI_VERSION,
    a_set_int, a_get_int,
    a_set_string, a_get_string,
    a_has, a_remove,
    a_set_float, a_get_float,
    a_set_session_int, a_get_session_int,
  };

  // NEUI_API_CLIPBOARD: convenience text APIs delegate to the shared
  // NSPasteboard helpers in hosts/shared/macos/clipboard_macos.h. v1 only
  // round-trips text/plain; the item-based API stays a stub until a
  // multi-format use case appears (matches the win32 / xpl shape).
  static int  NEUI_ABI c_set_text(neui_session_t /*sess*/, const char* utf8)
  {
    return neui_detail::clipboard_set_text_macos(utf8, utf8 ? (uint32_t)strlen(utf8) : 0)
             ? 1 : 0;
  }
  static int  NEUI_ABI c_get_text(neui_session_t /*sess*/, char* buf, int buflen)
  {
    return neui_detail::clipboard_get_text_macos(buf, buflen);
  }
  static bool NEUI_ABI c_has_text(neui_session_t /*sess*/)
  {
    return neui_detail::clipboard_has_text_macos();
  }
  static neui_clipboard_item_t NEUI_ABI c_read       (neui_session_t)           { return neui_clipboard_item_none; }
  static neui_clipboard_item_t NEUI_ABI c_create_item(neui_session_t)           { return neui_clipboard_item_none; }
  static void NEUI_ABI c_release(neui_session_t, neui_clipboard_item_t)         {}
  static int  NEUI_ABI c_write  (neui_session_t, neui_clipboard_item_t)         { return 0; }
  static int  NEUI_ABI c_item_set_format(neui_session_t, neui_clipboard_item_t,
                                          const char*, const void*, uint32_t)   { return 0; }
  static int  NEUI_ABI c_item_get_format(neui_session_t, neui_clipboard_item_t,
                                          const char*, void*, int)              { return 0; }
  static bool NEUI_ABI c_item_has_format(neui_session_t, neui_clipboard_item_t,
                                          const char*)                          { return false; }

  neui_clipboard_api_t clipboard_api = {
    NEUI_VERSION,
    c_set_text, c_get_text, c_has_text,
    c_read, c_create_item, c_release,
    c_write,
    c_item_set_format, c_item_get_format, c_item_has_format,
  };

  static int NEUI_ABI cmd_invoke_focused(neui_session_t, uint32_t)              { return 0; }
  static int NEUI_ABI cmd_invoke        (neui_session_t, neui_widget_t, uint32_t) { return 0; }

  neui_commands_api_t commands_api = {
    NEUI_VERSION,
    cmd_invoke_focused, cmd_invoke,
  };

  // Asset API. Session-scoped slot table backing the public
  // neui_asset_api_t. Loaded outside the paint loop; per-paint GPU
  // upload happens lazily inside macos_painter_draw_asset_thunk.
  // Mirrors hosts/win32/widgets.cpp::as_* almost verbatim - the only
  // host-specific bit is using neui_cg_backend::get_backend() in
  // as_destroy so the manager can release any per-ctx CGImage caches
  // the bitmap accumulated across paint contexts.

  static neui_asset_t pack_asset_macos(uint32_t session_id, uint32_t slot)
  {
    return { ((session_id & 0xffff) << 16) | (slot & 0xffff) };
  }

  static neui_asset_t NEUI_ABI a_create_bitmap(neui_session_t session,
                                                 uint32_t width_px,
                                                 uint32_t height_px,
                                                 const uint8_t* bgra,
                                                 float scale)
  {
    auto* s = get_session(session);
    if (!s) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_bitmap(width_px, height_px,
                                                        bgra, scale);
    if (slot == 0) return asset_none;
    return pack_asset_macos(s->session_id(), slot);
  }

  static neui_asset_t NEUI_ABI a_create_from_file(neui_session_t session,
                                                    const char* path_utf8)
  {
    auto* s = get_session(session);
    if (!s || !path_utf8) return asset_none;
    // Pick the highest backing-scale across the session's frames as the
    // preference for @Nx variant resolution. Falls back to the main
    // screen's backing scale when no frame has been created yet.
    float scale = 1.0f;
    if (NSScreen.mainScreen) {
      float main_scale = (float)NSScreen.mainScreen.backingScaleFactor;
      if (main_scale > scale) scale = main_scale;
    }
    uint32_t slot = s->_asset_manager.allocate_from_file(path_utf8, scale);
    if (slot == 0) return asset_none;
    return pack_asset_macos(s->session_id(), slot);
  }

  static void NEUI_ABI a_destroy(neui_session_t session, neui_asset_t asset)
  {
    auto* s = get_session(session);
    if (!s) return;
    if (asset.id == asset_none.id) return;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return;
    s->_asset_manager.release_slot(asset.id & 0xffff,
                                    neui_cg_backend::get_backend());
  }

  static bool NEUI_ABI a_get_size(neui_session_t session, neui_asset_t asset,
                                    float* out_w, float* out_h)
  {
    auto* s = get_session(session);
    if (!s) return false;
    if (asset.id == asset_none.id) return false;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return false;
    auto* e = s->_asset_manager.get_slot(asset.id & 0xffff);
    if (!e || e->scale <= 0.0f) return false;
    if (out_w) *out_w = static_cast<float>(e->width_px)  / e->scale;
    if (out_h) *out_h = static_cast<float>(e->height_px) / e->scale;
    return true;
  }

  static neui_asset_kind_t NEUI_ABI a_get_kind(neui_session_t session,
                                                 neui_asset_t asset)
  {
    auto* s = get_session(session);
    if (!s) return NEUI_ASSET_KIND_NONE;
    if (asset.id == asset_none.id) return NEUI_ASSET_KIND_NONE;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff))
      return NEUI_ASSET_KIND_NONE;
    auto* e = s->_asset_manager.get_slot(asset.id & 0xffff);
    return e ? e->kind : NEUI_ASSET_KIND_NONE;
  }

  static neui_asset_t NEUI_ABI a_create_compound(neui_session_t session)
  {
    auto* s = get_session(session);
    if (!s) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_compound();
    if (slot == 0) return asset_none;
    return pack_asset_macos(s->session_id(), slot);
  }

  neui_asset_api_t asset_api = {
    NEUI_VERSION,
    a_create_bitmap,
    a_create_from_file,
    a_destroy,
    a_get_size,
    a_get_kind,
    a_create_compound,
  };

  // Compound API. Mutators dispatch to the shared mutator helpers in
  // hosts/shared/compound.h; the host-side concern here is handle
  // validation, looking up the MacOSAssetEntry's compound storage, and
  // invalidating attached CUSTOMDRAW widgets after each mutation.
  // Mirror of hosts/win32/widgets.cpp's co_* family.

  static neui_detail::CompoundLayer*
  resolve_layer_macos(neui_session_t session, neui_asset_t asset,
                       neui_compound_layer_t layer, Session*& out_session)
  {
    out_session = nullptr;
    auto* s = get_session(session);
    if (!s) return nullptr;
    if (asset.id == asset_none.id) return nullptr;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return nullptr;
    uint32_t asset_slot = asset.id & 0xffff;
    if (neui_detail::compound_layer_asset_slot(layer) != asset_slot) return nullptr;
    auto* e = s->_asset_manager.get_slot(asset_slot);
    if (!e || e->kind != NEUI_ASSET_KIND_COMPOUND || !e->compound) return nullptr;
    out_session = s;
    return neui_detail::compound_get_layer(*e->compound,
                                            neui_detail::compound_layer_slot(layer));
  }

  static neui_detail::CompoundAsset*
  resolve_compound_macos(neui_session_t session, neui_asset_t asset,
                          Session*& out_session)
  {
    out_session = nullptr;
    auto* s = get_session(session);
    if (!s) return nullptr;
    if (asset.id == asset_none.id) return nullptr;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return nullptr;
    auto* e = s->_asset_manager.get_slot(asset.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_COMPOUND || !e->compound) return nullptr;
    out_session = s;
    return e->compound.get();
  }

  static neui_compound_layer_t NEUI_ABI co_add_layer(neui_session_t session,
                                                       neui_asset_t asset,
                                                       neui_compound_layer_kind_t kind,
                                                       int z)
  {
    Session* s = nullptr;
    auto* ca = resolve_compound_macos(session, asset, s);
    if (!ca) return compound_layer_none;
    uint32_t asset_slot = asset.id & 0xffff;
    uint32_t slot = neui_detail::compound_add_layer(*ca, kind, z);
    s->invalidate_widgets_with_compound(asset.id);
    return neui_detail::pack_compound_layer(asset_slot, slot);
  }

  static void NEUI_ABI co_remove_layer(neui_session_t session, neui_asset_t asset,
                                         neui_compound_layer_t layer)
  {
    Session* s = nullptr;
    auto* ca = resolve_compound_macos(session, asset, s);
    if (!ca) return;
    if (neui_detail::compound_layer_asset_slot(layer) != (asset.id & 0xffff)) return;
    neui_detail::compound_remove_layer(*ca, neui_detail::compound_layer_slot(layer));
    s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_clear(neui_session_t session, neui_asset_t asset)
  {
    Session* s = nullptr;
    auto* ca = resolve_compound_macos(session, asset, s);
    if (!ca) return;
    neui_detail::compound_clear(*ca);
    s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_set_z(neui_session_t session, neui_asset_t asset,
                                  neui_compound_layer_t layer, int z)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_macos(session, asset, layer, s);
    if (!L) return;
    L->z = z;
    s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_set_anchor(neui_session_t session, neui_asset_t asset,
                                       neui_compound_layer_t layer,
                                       neui_anchor_t parent_anchor,
                                       neui_anchor_t self_anchor)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_macos(session, asset, layer, s);
    if (!L) return;
    L->parent_anchor = parent_anchor;
    L->self_anchor   = self_anchor;
    s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_set_int(neui_session_t session, neui_asset_t asset,
                                    neui_compound_layer_t layer,
                                    const char* prop, int value)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_macos(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_set_int(*L, prop, value);
    s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_set_float(neui_session_t session, neui_asset_t asset,
                                      neui_compound_layer_t layer,
                                      const char* prop, float value)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_macos(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_set_float(*L, prop, value);
    s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_set_string(neui_session_t session, neui_asset_t asset,
                                       neui_compound_layer_t layer,
                                       const char* prop, const char* value)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_macos(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_set_string(*L, prop, value);
    s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_set_asset(neui_session_t session, neui_asset_t asset,
                                      neui_compound_layer_t layer,
                                      const char* prop, neui_asset_t value)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_macos(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_set_asset(*L, prop, value);
    s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_bind(neui_session_t session, neui_asset_t asset,
                                 neui_compound_layer_t layer,
                                 const char* prop, const char* attr_key,
                                 float scale, float offset)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_macos(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_bind(*L, prop, attr_key, scale, offset);
    s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_bind_asset(neui_session_t session, neui_asset_t asset,
                                       neui_compound_layer_t layer,
                                       const char* prop, const char* attr_key)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_macos(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_bind_asset(*L, prop, attr_key);
    s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_unbind(neui_session_t session, neui_asset_t asset,
                                   neui_compound_layer_t layer, const char* prop)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_macos(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_unbind(*L, prop);
    s->invalidate_widgets_with_compound(asset.id);
  }

  neui_compound_api_t compound_api = {
    NEUI_VERSION,
    co_add_layer,
    co_remove_layer,
    co_clear,
    co_set_z,
    co_set_anchor,
    co_set_int,
    co_set_float,
    co_set_string,
    co_set_asset,
    co_bind,
    co_bind_asset,
    co_unbind,
  };

} // namespace macos_host
