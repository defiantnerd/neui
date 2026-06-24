// Native macOS host - neui_widget_api_t / neui_items_api_t / neui_tree_api_t /
// neui_attr_api_t / neui_clipboard_api_t / neui_commands_api_t.
//
// Wires widget_create / widget_destroy (tree slot allocation) and routes
// widget_show through Session. NSWindow plumbing for APPWINDOW lives in
// window.mm; widget_show's per-type switch covers LABEL / BUTTON / INPUTBOX / ...
//
// Shape mirror of hosts/win32/widgets.cpp.

#import <AppKit/AppKit.h>

#include <fstream>
#include <string>
#include "host.h"
#include "checkbox_image.h"
#include "../shared/macos/clipboard_macos.h"
#include "../shared/macos/dnd_source_macos.h"
#include "../shared/macos/menubar_macos.h"
#include "../shared/compound.h"
#include "../../backends/cg/cg_backend.h"

// One TU emits the @implementation NeuiToastMacOS body. Other TUs that
// include toast_macos.h see only the @interface declaration.
#define NEUI_TOAST_MACOS_IMPLEMENTATION
#include "../shared/macos/toast_macos.h"
#include "../shared/macos/message_box_macos.h"

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

  // Defined in window.mm. Realizes a widget's NSView immediately if its
  // containing frame is already shown (post-show dynamic creation); no-op
  // before widget_show (deferred to create_descendants_native). Declared
  // here so widget_create below can call it.
  void realize_widget_macos(Session* s, uint32_t idx);

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
      !strcmp(type, NEUI_W_CUSTOMDRAW)||
      !strcmp(type, NEUI_W_GRID)     ||
      // TABVIEW emits NEUI_EVENT_TAB_* on chip clicks; its painted view runs
      // the hit-test itself (gated on `enabled`), but keep parity with the
      // other interactive types. TABPAGE is a chip-less SECTION - non-emit
      // like SECTION; its scroll input is gated on section_scroll_state.
      !strcmp(type, NEUI_W_TABVIEW));

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
      return IndexToWidget(_session_id, slot);
    }

    // If the containing frame is already on screen, realize this widget's
    // NSView now (post-show dynamic creation). Before widget_show there is
    // no realized ancestor, so this is a no-op and create_descendants_native
    // builds the view at show time. Mirror of the win32 host's immediate
    // child-HWND creation in widget_create.
    realize_widget_macos(this, slot);

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
  // Defined in window.mm. Pushes WidgetData::enabled into the live native
  // control ([NSControl setEnabled:] / NSTextView gating / painted-view
  // repaint). No-op until the control is created.
  void apply_enabled_native_macos (WidgetData& wd);
  // Defined in window.mm. Pushes WidgetData geometry into the live native
  // object (NSView frame / NSWindow content size + origin / painted-view
  // render-ctx resize). No-op until the native object exists.
  void apply_geometry_native_macos(WidgetData& wd);
  // Defined in window.mm. Blocking popup menu anchored to an NSView; returns
  // the 1-based pick (separators consume an index) or 0 on dismiss.
  int  run_popup_menu_macos(NSView* anchor, int x, int y, const char* const* items);
  // Defined in window.mm. Applies NEUI_ATTR_FONT_* to a native control's
  // NSFont (no-op for painted widgets + when no font attr is set).
  void apply_font_native_macos(WidgetData& wd);

  // Defined in window.mm. SECTION scrolling helpers; the attr setters
  // call them on NEUI_ATTR_SCROLL_MODE / CONTENT_WIDTH / CONTENT_HEIGHT
  // changes.
  void section_refresh_scroll_state_macos(WidgetData& wd);
  void section_apply_layout_changes_macos(WidgetData& sec);
  void section_create_body_view_macos(WidgetData& sec);
  void section_destroy_body_view_macos(WidgetData& sec);
  void section_reparent_children_macos(WidgetData& sec, bool to_body);
  void section_ensure_body_view_macos(WidgetData& sec);
  // Defined in window.mm. Used by the Scroll API external-commit path
  // (section_external_commit_macos): shuffle child NSView origins to the
  // committed scroll offset + fire NEUI_EVENT_SCROLL_CHANGED.
  void section_reposition_children_macos(WidgetData& sec);
  void section_notify_scroll_changed_macos(WidgetData& wd);

  // TABVIEW helpers (defined in window.mm). The TABS API + the attr setters
  // for chip-style changes call them.
  void tabview_collect_pages_macos(WidgetData& tv, std::vector<uint32_t>& out);
  void tabview_select_macos(WidgetData& tv, int new_index);
  void tabview_apply_page_geometry_macos(WidgetData& tv);

  // A TABPAGE is a chip-less SECTION (mirrors window.mm::is_section_like): the
  // section attr handlers treat both alike. Kept local to this TU.
  static inline bool is_section_like_w(const char* type)
  {
    return type && (!strcmp(type, NEUI_W_SECTION) ||
                    !strcmp(type, NEUI_W_TABPAGE));
  }
  // The TABVIEW parent of `wd` if `wd` is a TABPAGE, else nullptr. Used to
  // repaint the chip strip when a page's tab label / chip colours change.
  static inline WidgetData* tabview_parent_of_page(WidgetData& wd)
  {
    if (!wd.session || !wd.type || strcmp(wd.type, NEUI_W_TABPAGE) != 0)
      return nullptr;
    uint32_t pidx = wd.session->_widgets.get_parent(wd.index);
    if (!pidx || !wd.session->_widgets.exists(pidx)) return nullptr;
    auto& pw = wd.session->_widgets[pidx];
    if (pw.type && !strcmp(pw.type, NEUI_W_TABVIEW)) return &pw;
    return nullptr;
  }

  // True for the three font attribute keys. Used by the attr setters to
  // re-apply the native font + repaint painted text on a live change.
  static inline bool is_font_attr(const char* key)
  {
    return key && (!strcmp(key, NEUI_ATTR_FONT_FAMILY) ||
                   !strcmp(key, NEUI_ATTR_FONT_SIZE)   ||
                   !strcmp(key, NEUI_ATTR_FONT_WEIGHT));
  }

  // Pack a (session_id, slot) pair into a neui_asset_t handle. Defined
  // lower in this TU alongside the asset API; forward-declared here so the
  // IMAGE source helpers in widget_set_text / widget_set_asset can build a
  // handle for an internally-allocated slot.
  static neui_asset_t pack_asset_macos(uint32_t session_id, uint32_t slot);

  // Highest backing scale across the session's screens, used as the @Nx
  // variant preference when allocating an IMAGE asset from a path before a
  // view exists. Mirrors a_create_from_file's choice.
  static float preferred_asset_scale_macos()
  {
    float scale = 1.0f;
    if (NSScreen.mainScreen) {
      float ms = (float)NSScreen.mainScreen.backingScaleFactor;
      if (ms > scale) scale = ms;
    }
    return scale;
  }

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

    // Tear down the native control first - for painted views this also drops
    // the asset manager's per-ctx GPU cache for the view's render context
    // (release_native_control_macos -> release_context). Then free any
    // internally-owned IMAGE asset slot (client-supplied handles are left
    // for the client to destroy).
    if (w.native_control) release_native_control_macos(w);
    if (w.native_window)  release_native_window_macos (w);

    if (w.image_asset_owned && w.image_asset.id != asset_none.id) {
      _asset_manager.release_slot(w.image_asset.id & 0xffff,
                                   neui_cg_backend::get_backend());
      w.image_asset       = asset_none;
      w.image_asset_owned = false;
    }

    // A destroyed TABPAGE drops a tab: re-flow the parent TABVIEW (the
    // selected index may now be out of range) + repaint its chip strip after
    // the slot is freed below.
    uint32_t tabview_parent = 0;
    if (w.type && !strcmp(w.type, NEUI_W_TABPAGE)) {
      uint32_t pidx = _widgets.get_parent(index);
      if (pidx && _widgets.exists(pidx)) {
        auto& pw = _widgets[pidx];
        if (pw.type && !strcmp(pw.type, NEUI_W_TABVIEW))
          tabview_parent = pidx;
      }
    }

    _widgets.remove(index);

    if (tabview_parent && _widgets.exists(tabview_parent)) {
      auto& tv = _widgets[tabview_parent];
      tabview_apply_page_geometry_macos(tv);
      mark_widget_dirty_for_paint(tv);
    }
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

  static void NEUI_ABI w_hide(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    wd.visible = false;
    if (wd.native_window) {
      [(__bridge NSWindow*)wd.native_window orderOut:nil];
    } else if (wd.native_control) {
      id obj = (__bridge id)wd.native_control;
      if ([obj isKindOfClass:[NSView class]]) [(NSView*)obj setHidden:YES];
    }
  }
  static void NEUI_ABI w_set_pos(neui_session_t session, neui_widget_t widget,
                                  int x, int y, int width, int height)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    wd.x = x; wd.y = y; wd.width = width; wd.height = height;
    apply_geometry_native_macos(wd);
  }
  static void NEUI_ABI w_set_size(neui_session_t session, neui_widget_t widget,
                                   int width, int height)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    wd.width = width; wd.height = height;
    apply_geometry_native_macos(wd);
  }
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

  static void NEUI_ABI w_set_text(neui_session_t session, neui_widget_t widget, const char* text)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    wd.text = text ? text : "";

    // IMAGE: the text is a file path. Allocate an internally-owned bitmap
    // asset from it (releasing any prior owned slot first), exactly like the
    // win32 host's widget_set_text. The drawRect: IMAGE branch resolves
    // wd.image_asset against the session asset manager, so there is no
    // per-view bitmap cache any more. Deferred-safe: allocation doesn't need
    // the NSView to exist - @Nx variant resolution uses the screen scale.
    if (wd.type && !strcmp(wd.type, NEUI_W_IMAGE)) {
      auto* backend = neui_cg_backend::get_backend();
      if (wd.image_asset_owned && wd.image_asset.id != asset_none.id)
        s->_asset_manager.release_slot(wd.image_asset.id & 0xffff, backend);
      wd.image_asset       = asset_none;
      wd.image_asset_owned = false;
      if (!wd.text.empty()) {
        uint32_t slot = s->_asset_manager.allocate_from_file(
                          wd.text, preferred_asset_scale_macos());
        if (slot != 0) {
          wd.image_asset       = pack_asset_macos(s->session_id(), slot);
          wd.image_asset_owned = true;
        }
      }
      mark_widget_dirty_for_paint(wd);
      return;
    }

    NSString* ns = [NSString stringWithUTF8String:wd.text.c_str()];
    if (wd.native_window) {
      [(__bridge NSWindow*)wd.native_window setTitle:ns];
    } else if (wd.native_control) {
      widget_set_native_string(wd, ns);
      // SECTION: text is the title chip - repaint so it picks up the change.
      if (wd.type && !strcmp(wd.type, NEUI_W_SECTION))
        mark_widget_dirty_for_paint(wd);
      // TABPAGE: text is the tab label - repaint the parent TABVIEW strip.
      if (auto* tv = tabview_parent_of_page(wd)) mark_widget_dirty_for_paint(*tv);
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
  static neui_widget_t NEUI_ABI w_get_first_child (neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return widget_none;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return widget_none;
    uint32_t child = s->_widgets.child(i);
    if (child == 0 || !s->_widgets.exists(child)) return widget_none;
    return { s->_widgets[child].widget_id };
  }
  static neui_widget_t NEUI_ABI w_get_next_sibling(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return widget_none;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return widget_none;
    uint32_t next = s->_widgets.next(i);
    if (next == 0 || !s->_widgets.exists(next)) return widget_none;
    return { s->_widgets[next].widget_id };
  }
  static void NEUI_ABI w_set_focus(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    if (wd.native_window) {
      [(__bridge NSWindow*)wd.native_window makeKeyAndOrderFront:nil];
      return;
    }
    if (!wd.native_control) return;
    id obj = (__bridge id)wd.native_control;
    if (![obj isKindOfClass:[NSView class]]) return;
    NSView* v = (NSView*)obj;
    // NSScrollView-hosted controls (MULTILINE / LISTBOX / TREEVIEW): focus the
    // document view (the editable text / table), not the scroll container.
    NSView* target = v;
    if ([v isKindOfClass:[NSScrollView class]]) {
      NSView* doc = ((NSScrollView*)v).documentView;
      if (doc) target = doc;
    }
    [v.window makeFirstResponder:target];
  }
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
  // Content area in the widget's own coordinate space. macOS frames use the
  // global NSMenu (no in-frame band), so this is the full widget rect at a
  // (0, 0) origin - matching the Linux xpl host's menubar-less / native-menu case.
  static void  NEUI_ABI w_get_client_rect(neui_session_t session, neui_widget_t widget,
                                          int* x, int* y, int* w, int* h)
  {
    auto* s = get_session_for_widget(session, widget);
    int gw = 0, gh = 0;
    if (s) {
      uint32_t i = WidgetToIndex(widget);
      if (s->_widgets.exists(i)) { gw = s->_widgets[i].width; gh = s->_widgets[i].height; }
    }
    if (x) *x = 0; if (y) *y = 0; if (w) *w = gw; if (h) *h = gh;
  }
  static int   NEUI_ABI w_popup_menu(neui_session_t session, neui_widget_t anchor,
                                      int x, int y, const char* const* items)
  {
    auto* s = get_session_for_widget(session, anchor);
    if (!s || !items) return 0;
    uint32_t i = WidgetToIndex(anchor);
    if (!s->_widgets.exists(i)) return 0;
    auto& wd = s->_widgets[i];
    NSView* view = nil;
    if (wd.native_control) {
      id obj = (__bridge id)wd.native_control;
      if ([obj isKindOfClass:[NSView class]]) view = (NSView*)obj;
    }
    return run_popup_menu_macos(view, x, y, items);
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

  // Bind an asset handle to a widget. Two widget types consume it:
  //  - NEUI_W_IMAGE: the bitmap asset becomes the image source (last-set
  //    wins against set_text). Mutual-clear: binding an asset drops any
  //    set_text path; asset_none clears the source.
  //  - NEUI_W_CUSTOMDRAW: a NEUI_ASSET_KIND_COMPOUND switches the paint
  //    path from imperative WIDGET_PAINT dispatch to declarative
  //    compound-layer walk; asset_none clears the binding.
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

    if (!strcmp(wd.type, NEUI_W_IMAGE)) {
      // Release the previous internally-owned slot (only ours to free;
      // client-supplied handles stay owned by the client). Store the new
      // handle as not-owned and clear the path so the two sources don't
      // both resolve. Mirror of the win32 host.
      if (wd.image_asset_owned && wd.image_asset.id != asset_none.id)
        s->_asset_manager.release_slot(wd.image_asset.id & 0xffff,
                                        neui_cg_backend::get_backend());
      wd.image_asset       = asset;
      wd.image_asset_owned = false;
      wd.text.clear();
      mark_widget_dirty_for_paint(wd);
    }
    else if (!strcmp(wd.type, NEUI_W_CUSTOMDRAW)) {
      // Kind-route: BEHAVIOR -> input slot, otherwise -> compound visual slot.
      // asset_none clears the compound slot (matches the v1 contract for
      // IMAGE / CUSTOMDRAW).
      if (asset.id == asset_none.id) {
        wd.compound_asset = asset_none;
      } else {
        auto* entry = s->_asset_manager.get_slot(asset.id & 0xffff);
        if (entry && entry->kind == NEUI_ASSET_KIND_BEHAVIOR) {
          wd.behavior_asset = asset;
        } else if (entry && entry->kind == NEUI_ASSET_KIND_COMPONENT) {
          // COMPONENT bundles compound + behavior + defaults: attach both
          // slots and stamp defaults (keys the widget lacks; client pre-sets win).
          wd.compound_asset = entry->comp_compound;
          wd.behavior_asset = entry->comp_behavior;
          auto& bag = neui_detail::ensure_attrs(wd.attrs);
          for (const auto& d : entry->comp_defaults) {
            if (bag.has(d.key)) continue;
            switch (d.type) {
              case neui_detail::ComponentDefaultAttr::INT:    bag.set_int(d.key, d.ival); break;
              case neui_detail::ComponentDefaultAttr::FLOAT:  bag.set_float(d.key, d.fval); break;
              case neui_detail::ComponentDefaultAttr::STRING: bag.set_string(d.key, d.sval.c_str()); break;
            }
          }
        } else {
          wd.compound_asset = asset;
        }
      }
      mark_widget_dirty_for_paint(wd);
    }
    // Other widget types: no-op.
  }

  // Instantiate a CUSTOMDRAW from a COMPONENT asset (create + COMPONENT-aware
  // set_asset, which attaches both slots + stamps defaults). Mirrors win32.
  static neui_widget_t NEUI_ABI w_create_from_component(neui_session_t session,
      neui_widget_t parent, neui_asset_t component, int x, int y, int width, int height)
  {
    auto* s = get_session_for_widget(session, parent);
    if (!s || component.id == asset_none.id) return widget_none;
    if (((component.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return widget_none;
    auto* ce = s->_asset_manager.get_slot(component.id & 0xffff);
    if (!ce || ce->kind != NEUI_ASSET_KIND_COMPONENT) return widget_none;
    if (width  <= 0) width  = static_cast<int>(ce->comp_w + 0.5f);
    if (height <= 0) height = static_cast<int>(ce->comp_h + 0.5f);
    neui_widget_t w = w_create(session, parent, NEUI_W_CUSTOMDRAW, x, y, width, height, nullptr);
    if (w.id == widget_none.id) return widget_none;
    w_set_asset(session, w, component);
    return w;
  }

  static void NEUI_ABI w_set_enabled(neui_session_t session, neui_widget_t widget, bool enabled)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    if (wd.enabled == enabled) return;
    wd.enabled = enabled;
    // Push the flag into the live native control. If the control has not been
    // created yet (deferred until widget_show), create_native_for_widget
    // re-applies wd.enabled right after instantiation.
    apply_enabled_native_macos(wd);
  }
  static bool NEUI_ABI w_get_enabled(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return false;
    uint32_t i = WidgetToIndex(widget);
    if (s->_widgets.exists(i)) return s->_widgets[i].enabled;
    return false;
  }

  // -------------------------------------------------------------------------
  // Notify API (NEUI_API_NOTIFY) - toast + message box. Host-owned chrome
  // anchored to a frame, outside the widget tree.

  // Resolve `parent_window` to a top-level frame's NSWindow (APPWINDOW /
  // PLUGWINDOW / DIALOG); nil for anything else.
  static NSWindow* notify_frame_window(neui_session_t session,
                                        neui_widget_t parent_window)
  {
    auto* s = get_session_for_widget(session, parent_window);
    if (!s) return nil;
    uint32_t i = WidgetToIndex(parent_window);
    if (!s->_widgets.exists(i)) return nil;
    auto& wd = s->_widgets[i];
    if (!wd.native_window || !wd.type) return nil;
    bool ok = !strcmp(wd.type, NEUI_W_APPWINDOW) ||
              !strcmp(wd.type, NEUI_W_PLUGWINDOW) ||
              !strcmp(wd.type, NEUI_W_DIALOG);
    if (!ok) return nil;
    return (__bridge NSWindow*)wd.native_window;
  }

  static void NEUI_ABI notify_toast(neui_session_t session,
                                     neui_widget_t parent_window,
                                     const char* text)
  {
    NSWindow* win = notify_frame_window(session, parent_window);
    if (!win) return;
    neui_detail::toast_show_macos(win, text ? text : "");
  }

  static int NEUI_ABI notify_message_box(neui_session_t session,
                                          neui_widget_t parent_window,
                                          const char* text, const char* caption,
                                          uint32_t flags)
  {
    NSWindow* win = notify_frame_window(session, parent_window);
    if (!win) return 0;
    return neui_detail::message_box_macos(win, text, caption, flags);
  }

  neui_notify_api_t notify_api = {
    NEUI_VERSION,
    notify_toast,
    notify_message_box,
  };

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
    w_set_enabled,
    w_get_enabled,
    w_get_client_rect,
    w_create_from_component,
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
      // example's `ud == (void*)20` check on the Help -> About activation
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
      // (top -> leaves) - the example fits that. Three-level can layer in
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

  static void NEUI_ABI t_set_menu_cmd(neui_session_t session, neui_widget_t widget,
                                       neui_item_t item, uint32_t cmd)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    auto it = wd.tree_items.find(item.id);
    if (it != wd.tree_items.end()) it->second.menu_cmd = cmd;
  }

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
    // SECTION / TABPAGE: NEUI_ATTR_BACKGROUND drives the body fill colour.
    if (is_section_like_w(wd.type) && !strcmp(key, NEUI_ATTR_BACKGROUND)) {
      mark_widget_dirty_for_paint(wd);
      // The active page's background also drives the TABVIEW body fill +
      // active-chip colour, so repaint the parent strip too.
      if (auto* tv = tabview_parent_of_page(wd)) mark_widget_dirty_for_paint(*tv);
    }
    // SECTION / TABPAGE content extent override: rebuild layout + reposition
    // children so the scrollbar reflects the new content size.
    if (is_section_like_w(wd.type) &&
        (!strcmp(key, NEUI_ATTR_CONTENT_WIDTH) ||
         !strcmp(key, NEUI_ATTR_CONTENT_HEIGHT))) {
      section_apply_layout_changes_macos(wd);
    }
    // TABPAGE chip colours -> repaint the parent TABVIEW's strip.
    if (wd.type && !strcmp(wd.type, NEUI_W_TABPAGE) &&
        (!strcmp(key, NEUI_ATTR_TAB_CHIP_BG_COLOR) ||
         !strcmp(key, NEUI_ATTR_TAB_CHIP_TEXT_COLOR))) {
      if (auto* tv = tabview_parent_of_page(wd)) mark_widget_dirty_for_paint(*tv);
    }
    // TABVIEW style attrs (strip size, border, chip radius, strip bg) ->
    // re-flow + repaint. (TAB_CHIP_RADIUS / TAB_STRIP_BG_COLOR don't change
    // the body rect, but they alter the painted strip, so a repaint is still
    // needed to apply them live - matching win32.)
    if (wd.type && !strcmp(wd.type, NEUI_W_TABVIEW) &&
        (!strcmp(key, NEUI_ATTR_TAB_STRIP_SIZE) ||
         !strcmp(key, NEUI_ATTR_TAB_BORDER_COLOR) ||
         !strcmp(key, NEUI_ATTR_TAB_BORDER_WIDTH) ||
         !strcmp(key, NEUI_ATTR_TAB_CHIP_RADIUS) ||
         !strcmp(key, NEUI_ATTR_TAB_STRIP_BG_COLOR) ||
         !strcmp(key, NEUI_ATTR_BACKGROUND))) {
      tabview_apply_page_geometry_macos(wd);
      mark_widget_dirty_for_paint(wd);
    }
    // NEUI_ATTR_FONT_WEIGHT: re-apply native control font + repaint painted text.
    if (is_font_attr(key)) { apply_font_native_macos(wd); mark_widget_dirty_for_paint(wd); }
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
    // shared paint_section helper reads it each draw. align="none" /
    // empty band swap changes band_h -> layout rebuild + reposition
    // children. NEUI_ATTR_BACKGROUND is handled in a_set_int.
    if (is_section_like_w(wd.type) &&
        !strcmp(key, NEUI_ATTR_ALIGN_TEXT)) {
      // A chip may have just appeared (align none -> left/center/right) on a
      // non-scrolling section: create the body view + reparent children so
      // they shift below the band. Kept for the section's lifetime, so a
      // later flip back to "none" leaves children body-local (band_h -> 0
      // collapses the offset) rather than dropping them onto the chip.
      section_ensure_body_view_macos(wd);
      section_apply_layout_changes_macos(wd);
    }
    // SECTION scroll-mode change: allocate / drop scroll state, rebuild
    // layout + reposition children. The painted view's scrollWheel: /
    // mouseDown: routes through SectionScrollState's nullable pointer
    // so the live opt-in / opt-out works without view recreation.
    if (is_section_like_w(wd.type) &&
        !strcmp(key, NEUI_ATTR_SCROLL_MODE)) {
      section_refresh_scroll_state_macos(wd);
      // The inner body view is created the first time a section becomes
      // scrollable (or carries a chip band) and then KEPT for the section's
      // lifetime - including after switching back to "none". Keeping it
      // means children stay body-local (laid out below the chip band) and
      // clipped to the body rect in every mode; destroying it on a
      // flip-to-"none" dropped the children to section-local coords, so they
      // jumped up into the chip band and overpainted it. Children only need
      // re-parenting the first time the body view appears. Mirror of the
      // win32 host.
      section_ensure_body_view_macos(wd);
      // Switching scroll mode resets the scroll offset to 0,0. (When the
      // mode is "none" the scroll state is gone and the reposition below
      // already uses offset 0; this covers scroll->scroll transitions like
      // vertical->both.)
      if (wd.section_scroll_state) {
        auto& st = *wd.section_scroll_state;
        st.scroll_x = st.scroll_y = 0;
        st.kin_v = st.kin_h = neui_detail::ScrollKinetics{};
        st.kinetic_over_v = st.kinetic_over_h = false;
      }
      section_apply_layout_changes_macos(wd);
    }
    // TABVIEW: NEUI_ATTR_TAB_POSITION changes the strip edge + content body
    // rect, so re-flow the pages + repaint the strip.
    if (wd.type && !strcmp(wd.type, NEUI_W_TABVIEW) &&
        !strcmp(key, NEUI_ATTR_TAB_POSITION)) {
      tabview_apply_page_geometry_macos(wd);
      mark_widget_dirty_for_paint(wd);
    }
    // NEUI_ATTR_FONT_FAMILY: re-apply native control font + repaint painted text.
    if (is_font_attr(key)) { apply_font_native_macos(wd); mark_widget_dirty_for_paint(wd); }
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
    // NEUI_ATTR_FONT_SIZE: re-apply native control font + repaint painted text.
    if (is_font_attr(key)) { apply_font_native_macos(wd); mark_widget_dirty_for_paint(wd); }
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

  // NEUI_API_CLIPBOARD. Convenience text APIs delegate to the text-only
  // helpers in clipboard_macos.h; the item-based API rides on top of the
  // multi-format read/write helpers in the same header.
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
  static neui_data_item_t NEUI_ABI c_read(neui_session_t session)
  {
    auto* s = get_session(session);
    if (!s) return neui_data_item_none;
    uint32_t id = s->_data_items.allocate();
    auto* item = s->_data_items.get(id);
    if (!item) return neui_data_item_none;
    if (!neui_detail::clipboard_read_item_macos(*item)) {
      s->_data_items.release(id);
      return neui_data_item_none;
    }
    return { id };
  }
  static neui_data_item_t NEUI_ABI c_create_item(neui_session_t session)
  {
    auto* s = get_session(session);
    if (!s) return neui_data_item_none;
    return { s->_data_items.allocate() };
  }
  static void NEUI_ABI c_release(neui_session_t session, neui_data_item_t item)
  {
    auto* s = get_session(session);
    if (s) s->_data_items.release(item.id);
  }
  static int NEUI_ABI c_write(neui_session_t session, neui_data_item_t item)
  {
    auto* s = get_session(session);
    if (!s) return 0;
    auto* it = s->_data_items.get(item.id);
    if (!it) return 0;
    return neui_detail::clipboard_write_item_macos(*it) ? 1 : 0;
  }
  static int NEUI_ABI c_item_set_format(neui_session_t session, neui_data_item_t item,
                                         const char* mime, const void* data, uint32_t length)
  {
    auto* s = get_session(session);
    if (!s || !mime) return 0;
    auto* it = s->_data_items.get(item.id);
    if (!it) return 0;
    it->set_format(mime, data, length);
    return 1;
  }
  static int NEUI_ABI c_item_get_format(neui_session_t session, neui_data_item_t item,
                                         const char* mime, void* buf, int buflen)
  {
    auto* s = get_session(session);
    if (!s || !mime) return -1;
    auto* it = s->_data_items.get(item.id);
    if (!it) return -1;
    return it->get_format(mime, buf, buflen);
  }
  static bool NEUI_ABI c_item_has_format(neui_session_t session, neui_data_item_t item,
                                          const char* mime)
  {
    auto* s = get_session(session);
    if (!s || !mime) return false;
    auto* it = s->_data_items.get(item.id);
    return it && it->has_format(mime);
  }
  static int NEUI_ABI c_item_set_format_callback(neui_session_t session, neui_data_item_t item,
                                                  const char* mime,
                                                  neui_data_provider_t provider, void* userdata)
  {
    auto* s = get_session(session);
    if (!s || !mime || !provider) return 0;
    auto* it = s->_data_items.get(item.id);
    if (!it) return 0;
    it->set_format_provider(mime, provider, userdata);
    return 1;
  }

  neui_clipboard_api_t clipboard_api = {
    NEUI_VERSION,
    c_set_text, c_get_text, c_has_text,
    c_read, c_create_item, c_release,
    c_write,
    c_item_set_format, c_item_get_format, c_item_has_format,
    c_item_set_format_callback,
  };

  // -------------------------------------------------------------------------
  // DnD API (NEUI_API_DND). Drop-target only in v1.

  static uint32_t dnd_widget_to_index(neui_widget_t w) { return w.id & 0xffff; }

  static void NEUI_ABI d_set_drop_target(neui_session_t session,
                                          neui_widget_t widget, bool enable)
  {
    auto* s = get_session(session);
    if (!s) return;
    uint32_t idx = dnd_widget_to_index(widget);
    if (!s->_widgets.exists(idx)) return;
    s->_widgets[idx].drop_target = enable;
  }

  static bool NEUI_ABI d_get_drop_target(neui_session_t session,
                                          neui_widget_t widget)
  {
    auto* s = get_session(session);
    if (!s) return false;
    uint32_t idx = dnd_widget_to_index(widget);
    if (!s->_widgets.exists(idx)) return false;
    return s->_widgets[idx].drop_target;
  }

  static void NEUI_ABI d_set_accepted_formats(neui_session_t session,
                                               neui_widget_t widget,
                                               const char* const* mimes,
                                               int count)
  {
    auto* s = get_session(session);
    if (!s) return;
    uint32_t idx = dnd_widget_to_index(widget);
    if (!s->_widgets.exists(idx)) return;
    auto& w = s->_widgets[idx];
    w.accepted_mimes.clear();
    if (mimes && count > 0) {
      w.accepted_mimes.reserve(static_cast<size_t>(count));
      for (int i = 0; i < count; ++i) {
        if (mimes[i]) w.accepted_mimes.emplace_back(mimes[i]);
      }
    }
  }

  static void NEUI_ABI d_accept(neui_session_t session,
                                 neui_dnd_action_t action)
  {
    auto* s = get_session(session);
    if (!s) return;
    if (!s->_in_dnd_dispatch) return;
    s->_last_accepted_action = static_cast<uint32_t>(action);
  }

  // Walk up from source_widget to the owning frame and return its
  // NSWindow* (native_window). Returns nil if there is no frame ancestor.
  static NSWindow* find_owning_nswindow_macos(Session* s, uint32_t widget_idx)
  {
    auto parents = s->_widgets.get_all_parents(widget_idx);
    for (auto p : parents) {
      if (p == 0) continue;
      if (!s->_widgets.exists(p)) continue;
      auto& wd = s->_widgets[p];
      if (wd.native_window) return (__bridge NSWindow*)wd.native_window;
    }
    // The source widget itself might be the frame.
    if (s->_widgets.exists(widget_idx)) {
      auto& wd = s->_widgets[widget_idx];
      if (wd.native_window) return (__bridge NSWindow*)wd.native_window;
    }
    return nil;
  }

  // Shared worker for both public entry points. asset_none + (-1, -1) =
  // no preview.
  static neui_dnd_action_t d_begin_drag_impl(neui_session_t session,
                                               neui_widget_t source_widget,
                                               neui_data_item_t payload,
                                               uint32_t allowed_actions,
                                               neui_asset_t preview_asset,
                                               int hot_x, int hot_y)
  {
    auto* s = get_session_for_widget(session, source_widget);
    if (!s) return NEUI_DND_ACTION_NONE;
    if (s->_in_dnd_dispatch) return NEUI_DND_ACTION_NONE;    // no re-entry
    if (s->_drag_source_active) return NEUI_DND_ACTION_NONE; // one drag at a time
    auto* item = s->_data_items.get(payload.id);
    if (!item) return NEUI_DND_ACTION_NONE;
    NSWindow* win = find_owning_nswindow_macos(s, dnd_widget_to_index(source_widget));
    if (!win) return NEUI_DND_ACTION_NONE;
    NSView* cv = [win contentView];
    if (!cv) return NEUI_DND_ACTION_NONE;

    // Resolve preview asset to an NSImage if one was supplied AND it
    // belongs to this session AND it has displayable pixels.
    NSImage* preview = nil;
    if (preview_asset.id != asset_none.id) {
      uint32_t a_sess = (preview_asset.id >> 16) & 0xffff;
      if (a_sess == (s->session_id() & 0xffff)) {
        const uint8_t* bgra = nullptr;
        uint32_t       w_px = 0, h_px = 0;
        float          scale = 1.0f;
        if (s->_asset_manager.get_pixels_for_export(preview_asset.id & 0xffff,
                                                      &bgra, &w_px, &h_px,
                                                      &scale)) {
          preview = neui_detail::macos_make_drag_nsimage(bgra, w_px, h_px, scale);
        }
      }
    }

    s->_drag_source_active = true;
    uint32_t r = neui_detail::macos_run_drag_source(cv, *item, allowed_actions,
                                                      preview, hot_x, hot_y);
    s->_drag_source_active = false;
    return static_cast<neui_dnd_action_t>(r);
  }

  static neui_dnd_action_t NEUI_ABI d_begin_drag(neui_session_t session,
                                                  neui_widget_t source_widget,
                                                  neui_data_item_t payload,
                                                  uint32_t allowed_actions)
  {
    return d_begin_drag_impl(session, source_widget, payload,
                              allowed_actions, asset_none, -1, -1);
  }

  static neui_dnd_action_t NEUI_ABI d_begin_drag_with_preview(
                                                  neui_session_t session,
                                                  neui_widget_t source_widget,
                                                  neui_data_item_t payload,
                                                  uint32_t allowed_actions,
                                                  const neui_drag_preview_t* preview)
  {
    if (!preview) {
      return d_begin_drag_impl(session, source_widget, payload,
                                allowed_actions, asset_none, -1, -1);
    }
    return d_begin_drag_impl(session, source_widget, payload,
                              allowed_actions, preview->image,
                              preview->hot_x, preview->hot_y);
  }

  neui_dnd_api_t dnd_api = {
    NEUI_VERSION,
    d_set_drop_target,
    d_get_drop_target,
    d_set_accepted_formats,
    d_accept,
    d_begin_drag,
    d_begin_drag_with_preview,
  };

  // Non-static wrapper so the BehaviorDispatchCtx::begin_drag callback wired
  // in window.mm's make_behavior_ctx_macos can reach the (file-static)
  // d_begin_drag_with_preview from a sibling TU.
  uint32_t macos_behavior_begin_drag(void* host_data,
                                       neui_data_item_t item,
                                       uint32_t allowed_actions,
                                       uint32_t preview_image,
                                       int hot_x, int hot_y)
  {
    auto* wd = static_cast<WidgetData*>(host_data);
    if (!wd) return NEUI_DND_ACTION_NONE;
    neui_session_t sess = { wd->session_id };
    neui_widget_t  wid  = { wd->widget_id };
    neui_drag_preview_t preview = { { preview_image }, hot_x, hot_y };
    return static_cast<uint32_t>(
      d_begin_drag_with_preview(sess, wid, item, allowed_actions,
                                  preview_image ? &preview : nullptr));
  }

  // Route a built-in command (NEUI_CMD_*) to the key window's first
  // responder via AppKit's standard editing actions. cut/copy/paste/
  // select-all/delete map to NSResponder/NSText selectors; undo/redo go
  // through the responder's NSUndoManager. Returns true if something handled
  // it. Non-static so the menu-pick router in window.mm can call it.
  // Mirror of the win32 host's Session::invoke_focused_command.
  bool invoke_focused_command_macos(uint32_t cmd)
  {
    NSWindow* win = [NSApp keyWindow];
    if (!win) return false;

    if (cmd == NEUI_CMD_UNDO || cmd == NEUI_CMD_REDO) {
      NSResponder* r = win.firstResponder;
      NSUndoManager* um = nil;
      if (r && [r respondsToSelector:@selector(undoManager)])
        um = [(id)r undoManager];
      if (!um) return false;
      if (cmd == NEUI_CMD_UNDO) { if (!um.canUndo) return false; [um undo]; return true; }
      else                      { if (!um.canRedo) return false; [um redo]; return true; }
    }

    SEL sel = nil;
    switch (cmd) {
      case NEUI_CMD_CUT:        sel = @selector(cut:);       break;
      case NEUI_CMD_COPY:       sel = @selector(copy:);      break;
      case NEUI_CMD_PASTE:      sel = @selector(paste:);     break;
      case NEUI_CMD_SELECT_ALL: sel = @selector(selectAll:); break;
      case NEUI_CMD_DELETE:     sel = @selector(delete:);    break;
      default: return false;  // NONE / unknown / >= USER_BASE: client's job
    }
    return [NSApp sendAction:sel to:nil from:nil] ? true : false;
  }

  static int NEUI_ABI cmd_invoke_focused(neui_session_t /*session*/, uint32_t cmd)
  {
    return invoke_focused_command_macos(cmd) ? 1 : 0;
  }
  static int NEUI_ABI cmd_invoke(neui_session_t session, neui_widget_t widget, uint32_t cmd)
  {
    // Focus the target widget, then run the command against the now-current
    // first responder (mirrors the win32 host's invoke_command).
    w_set_focus(session, widget);
    return invoke_focused_command_macos(cmd) ? 1 : 0;
  }

  neui_commands_api_t commands_api = {
    NEUI_VERSION,
    cmd_invoke_focused, cmd_invoke,
  };

  // ---------------------------------------------------------------------------
  // Scroll API (NEUI_API_SCROLL)
  // ---------------------------------------------------------------------------

  // External commit: writes (nx, ny) into the section's state, resets the
  // per-axis kinetics integrator (so an in-flight bounce yields on its
  // next tick + a later wheel doesn't snap back from a stale raw_px),
  // repositions children, invalidates, and fires SCROLL_CHANGED. The
  // bounce timer self-cancels next tick when last_commit_px diverges.
  static void section_external_commit_macos(WidgetData& sec, int nx, int ny)
  {
    if (!sec.section_scroll_state) return;
    auto& st = *sec.section_scroll_state;
    auto& L  = sec.section_last_layout;
    int max_x = st.content_w - L.body_w; if (max_x < 0) max_x = 0;
    int max_y = st.content_h - L.body_h; if (max_y < 0) max_y = 0;
    if (nx < 0)     nx = 0;
    if (nx > max_x) nx = max_x;
    if (ny < 0)     ny = 0;
    if (ny > max_y) ny = max_y;
    if (st.scroll_x == nx && st.scroll_y == ny) return;
    st.scroll_x = nx;
    st.scroll_y = ny;
    st.kin_v.raw_px            = (double)ny;
    st.kin_v.last_commit_px    = ny;
    st.kin_v.suppress_momentum = true;
    st.kin_h.raw_px            = (double)nx;
    st.kin_h.last_commit_px    = nx;
    st.kin_h.suppress_momentum = true;
    st.kinetic_over_v = false;
    st.kinetic_over_h = false;
    section_reposition_children_macos(sec);
    mark_widget_dirty_for_paint(sec);
    section_notify_scroll_changed_macos(sec);
  }

  static int NEUI_ABI scroll_set(neui_session_t session, neui_widget_t widget,
                                  int scroll_x, int scroll_y)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return 0;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return 0;
    auto& wd = s->_widgets[idx];
    if (!wd.section_scroll_state) return 0;
    section_external_commit_macos(wd, scroll_x, scroll_y);
    return 1;
  }

  static int NEUI_ABI scroll_get(neui_session_t session, neui_widget_t widget,
                                  int* out_x, int* out_y)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return 0;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return 0;
    auto& wd = s->_widgets[idx];
    if (!wd.section_scroll_state) return 0;
    if (out_x) *out_x = wd.section_scroll_state->scroll_x;
    if (out_y) *out_y = wd.section_scroll_state->scroll_y;
    return 1;
  }

  // Walk up from widget_idx to the nearest ancestor with a scrolling
  // SECTION state. Accumulates the widget's body-local position within
  // that ancestor (sum of widget's own x/y + intermediate parents' x/y).
  static uint32_t find_scrolling_section_ancestor_macos(Session* s,
                                                          uint32_t widget_idx,
                                                          int& out_x, int& out_y)
  {
    out_x = 0; out_y = 0;
    if (!s || !s->_widgets.exists(widget_idx)) return 0;
    auto& wd0 = s->_widgets[widget_idx];
    out_x = wd0.x;
    out_y = wd0.y;
    uint32_t cur = s->_widgets.get_parent(widget_idx);
    while (cur != 0 && cur != neui_detail::knone.id && s->_widgets.exists(cur)) {
      auto& cw = s->_widgets[cur];
      if (cw.section_scroll_state) return cur;
      out_x += cw.x;
      out_y += cw.y;
      cur = s->_widgets.get_parent(cur);
    }
    return 0;
  }

  // Body of Session::ensure_widget_visible - lives in widgets.mm next to
  // the section helpers it uses. The public API delegates here.
  void Session::ensure_widget_visible(uint32_t widget_idx)
  {
    using namespace neui_detail;
    if (!_widgets.exists(widget_idx)) return;
    auto& wd0 = _widgets[widget_idx];
    int rect_x = 0, rect_y = 0;
    uint32_t sec_idx = find_scrolling_section_ancestor_macos(this, widget_idx,
                                                               rect_x, rect_y);
    if (sec_idx == 0) return;
    auto& sec = _widgets[sec_idx];
    auto& st  = *sec.section_scroll_state;
    auto& L   = sec.section_last_layout;
    int nx, ny;
    compute_ensure_visible(rect_x, rect_y, wd0.width, wd0.height,
                            L.body_w, L.body_h,
                            st.content_w, st.content_h,
                            st.scroll_x, st.scroll_y,
                            nx, ny);
    section_external_commit_macos(sec, nx, ny);
  }

  static int NEUI_ABI scroll_ensure_visible(neui_session_t session,
                                              neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return 0;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return 0;
    s->ensure_widget_visible(idx);
    return 1;
  }

  neui_scroll_api_t scroll_api = {
    NEUI_VERSION,
    scroll_set,
    scroll_get,
    scroll_ensure_visible,
  };

  // -------------------------------------------------------------------------
  // Tabs API (NEUI_API_TABS) - selection control over a TABVIEW. Tabs are the
  // TABVIEW's NEUI_W_TABPAGE children in creation order. Mirror of the xpl
  // host's tabview_from + the 5 thin methods.

  // Resolve `widget` to a TABVIEW WidgetData* (valid, this session, type
  // TABVIEW) or nullptr.
  static WidgetData* tabview_from_macos(neui_session_t session,
                                        neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return nullptr;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return nullptr;
    auto& wd = s->_widgets[idx];
    if (!wd.type || strcmp(wd.type, NEUI_W_TABVIEW) != 0) return nullptr;
    return &wd;
  }

  static uint32_t NEUI_ABI tabs_count(neui_session_t session, neui_widget_t tabview)
  {
    WidgetData* tv = tabview_from_macos(session, tabview);
    if (!tv) return 0;
    std::vector<uint32_t> pages; tabview_collect_pages_macos(*tv, pages);
    return (uint32_t)pages.size();
  }

  static uint32_t NEUI_ABI tabs_get_selected(neui_session_t session,
                                             neui_widget_t tabview)
  {
    WidgetData* tv = tabview_from_macos(session, tabview);
    if (!tv) return NEUI_ITEM_NONE;
    std::vector<uint32_t> pages; tabview_collect_pages_macos(*tv, pages);
    if (pages.empty()) return NEUI_ITEM_NONE;
    int sel = tv->tab_selected;
    if (sel < 0) sel = 0;
    if (sel >= (int)pages.size()) sel = (int)pages.size() - 1;
    return (uint32_t)sel;
  }

  static void NEUI_ABI tabs_set_selected(neui_session_t session,
                                         neui_widget_t tabview, uint32_t index)
  {
    WidgetData* tv = tabview_from_macos(session, tabview);
    if (!tv) return;
    // Clamp huge / sentinel indices (e.g. NEUI_ITEM_NONE) to a representable
    // int so the cast doesn't wrap negative - per the documented "clamped to
    // [0, count)", an out-of-range index selects the LAST tab, not the first.
    int ni = index > 0x7fffffffu ? 0x7fffffff : (int)index;
    tabview_select_macos(*tv, ni);
  }

  static neui_widget_t NEUI_ABI tabs_get_page(neui_session_t session,
                                              neui_widget_t tabview, uint32_t index)
  {
    WidgetData* tv = tabview_from_macos(session, tabview);
    if (!tv) return widget_none;
    std::vector<uint32_t> pages; tabview_collect_pages_macos(*tv, pages);
    if (index >= pages.size()) return widget_none;
    return neui_widget_t{ tv->session->_widgets[pages[index]].widget_id };
  }

  static uint32_t NEUI_ABI tabs_get_index(neui_session_t session,
                                          neui_widget_t tabview, neui_widget_t page)
  {
    WidgetData* tv = tabview_from_macos(session, tabview);
    if (!tv) return NEUI_ITEM_NONE;
    std::vector<uint32_t> pages; tabview_collect_pages_macos(*tv, pages);
    for (uint32_t i = 0; i < pages.size(); ++i)
      if (tv->session->_widgets[pages[i]].widget_id == page.id)
        return i;
    return NEUI_ITEM_NONE;
  }

  neui_tabs_api_t tabs_api = {
    NEUI_VERSION,
    tabs_count,
    tabs_get_selected,
    tabs_set_selected,
    tabs_get_page,
    tabs_get_index,
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

  // Best-guess @Nx scale for a file load: the main screen's backing scale.
  // Falls back to 1.0 when no screen is available.
  static float best_asset_scale_macos()
  {
    float scale = 1.0f;
    if (NSScreen.mainScreen) {
      float main_scale = (float)NSScreen.mainScreen.backingScaleFactor;
      if (main_scale > scale) scale = main_scale;
    }
    return scale;
  }

  static neui_asset_t NEUI_ABI a_create_from_file(neui_session_t session,
                                                    const char* path_utf8)
  {
    auto* s = get_session(session);
    if (!s || !path_utf8) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_from_file(path_utf8,
                                                         best_asset_scale_macos());
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
    if (!e) return false;
    if (e->kind == NEUI_ASSET_KIND_COMPONENT) {
      if (out_w) *out_w = e->comp_w;
      if (out_h) *out_h = e->comp_h;
      return true;
    }
    if (e->scale <= 0.0f) return false;
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

  static neui_asset_t NEUI_ABI a_create_behavior(neui_session_t session)
  {
    auto* s = get_session(session);
    if (!s) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_behavior();
    if (slot == 0) return asset_none;
    return pack_asset_macos(s->session_id(), slot);
  }

  // Defined in window.mm - same thunk WIDGET_PAINT installs into the
  // painter, lifted here so nested draw_asset works from inside a
  // surface paint.
  void NEUI_ABI macos_painter_draw_asset_thunk(void* host_token,
                                                 neui_render_backend_t* backend,
                                                 neui_render_ctx_t ctx,
                                                 neui_asset_t asset,
                                                 float x, float y,
                                                 float w, float h,
                                                 uint32_t frame,
                                                 uint32_t tint);

  static neui_asset_t NEUI_ABI a_create_surface(neui_session_t session,
                                                  float width_logical,
                                                  float height_logical,
                                                  float scale)
  {
    auto* s = get_session(session);
    if (!s) return asset_none;
    if (width_logical <= 0.0f || height_logical <= 0.0f) return asset_none;
    if (scale <= 0.0f) scale = 1.0f;
    uint32_t w_px = static_cast<uint32_t>(width_logical  * scale + 0.5f);
    uint32_t h_px = static_cast<uint32_t>(height_logical * scale + 0.5f);
    uint32_t slot = s->_asset_manager.allocate_surface(
      w_px, h_px, scale, neui_cg_backend::get_backend());
    if (slot == 0) return asset_none;
    return pack_asset_macos(s->session_id(), slot);
  }

  static void NEUI_ABI a_paint_surface(neui_session_t        session,
                                         neui_asset_t          surface,
                                         uint32_t              clear_argb,
                                         neui_surface_paint_fn fn,
                                         void*                 user)
  {
    auto* s = get_session(session);
    if (!s) return;
    if (surface.id == asset_none.id) return;
    if (((surface.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return;
    s->_asset_manager.paint_surface(surface.id & 0xffff, clear_argb, fn, user,
                                     neui_cg_backend::get_backend(),
                                     /*host_token*/ s,
                                     &macos_painter_draw_asset_thunk);
  }

  static neui_asset_t NEUI_ABI a_create_font(neui_session_t session,
                                              const uint8_t* data, uint32_t len)
  {
    auto* s = get_session(session);
    if (!s || !data || len == 0) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_font(
      data, len, neui_cg_backend::get_backend());
    if (slot == 0) return asset_none;
    return pack_asset_macos(s->session_id(), slot);
  }

  static neui_asset_t NEUI_ABI a_create_font_from_file(neui_session_t session,
                                                       const char* path_utf8)
  {
    auto* s = get_session(session);
    if (!s || !path_utf8) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_font_from_file(
      path_utf8, neui_cg_backend::get_backend());
    if (slot == 0) return asset_none;
    return pack_asset_macos(s->session_id(), slot);
  }

  static uint32_t NEUI_ABI a_get_font_family(neui_session_t session,
                                             neui_asset_t font,
                                             char* out_buf, uint32_t cap)
  {
    auto* s = get_session(session);
    if (!s || font.id == asset_none.id) return 0;
    if (((font.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return 0;
    return s->_asset_manager.get_font_family(font.id & 0xffff, out_buf, cap);
  }

  // Asset / compound / behavior tables (compound_api / behavior_api defined
  // later in this TU; asset_api just below). Forward-declared so the component
  // thunks can hand all three to build_component.
  extern neui_asset_api_t    asset_api;
  extern neui_compound_api_t compound_api;
  extern neui_behavior_api_t behavior_api;

  static void release_built_component_macos(neui_session_t session,
                                            neui_detail::BuiltComponent& built)
  {
    if (built.compound.id != asset_none.id) a_destroy(session, built.compound);
    if (built.behavior.id != asset_none.id) a_destroy(session, built.behavior);
    for (auto a : built.owned_assets) a_destroy(session, a);
  }

  static neui_asset_t NEUI_ABI a_create_component_from_string(
      neui_session_t session, const char* json, uint32_t len,
      const neui_component_env_t* env)
  {
    auto* s = get_session(session);
    if (!s || !json) return asset_none;
    neui_detail::ComponentApis apis;
    apis.asset    = &asset_api;
    apis.compound = &compound_api;
    apis.behavior = &behavior_api;
    neui_detail::BuiltComponent built =
        neui_detail::build_component(session, json, len, env, apis);
    if (!built.ok) { release_built_component_macos(session, built); return asset_none; }
    uint32_t slot = s->_asset_manager.allocate_component(built);
    if (slot == 0) { release_built_component_macos(session, built); return asset_none; }
    return pack_asset_macos(s->session_id(), slot);
  }

  static neui_asset_t NEUI_ABI a_create_component_from_file(
      neui_session_t session, const char* path_utf8,
      const neui_component_env_t* env)
  {
    auto* s = get_session(session);
    if (!s || !path_utf8) return asset_none;
    std::ifstream in(path_utf8, std::ios::binary);
    if (!in) return asset_none;
    std::string data((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    neui_component_env_t local{};
    const neui_component_env_t* use_env = env;
    static thread_local std::string base_keep;
    if (!env || !env->base_dir) {
      std::string p = path_utf8;
      size_t cut = p.find_last_of("/\\");
      base_keep = (cut == std::string::npos) ? std::string() : p.substr(0, cut);
      if (env) local = *env;
      local.base_dir = base_keep.c_str();
      use_env = &local;
    }
    return a_create_component_from_string(session, data.c_str(),
                                          static_cast<uint32_t>(data.size()), use_env);
  }

  static uint32_t NEUI_ABI a_component_param_count(neui_session_t session,
                                                   neui_asset_t component)
  {
    auto* s = get_session(session);
    if (!s || component.id == asset_none.id) return 0;
    if (((component.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return 0;
    auto* e = s->_asset_manager.get_slot(component.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_COMPONENT) return 0;
    return static_cast<uint32_t>(e->comp_params.size());
  }

  static bool NEUI_ABI a_component_param_at(neui_session_t session,
                                            neui_asset_t component,
                                            uint32_t index,
                                            neui_component_param_t* out)
  {
    auto* s = get_session(session);
    if (!s || !out || component.id == asset_none.id) return false;
    if (((component.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return false;
    auto* e = s->_asset_manager.get_slot(component.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_COMPONENT) return false;
    if (index >= e->comp_params.size()) return false;
    const auto& p = e->comp_params[index];
    out->key = p.key.c_str(); out->label = p.label.c_str();
    out->min = p.min; out->max = p.max; out->def = p.def;
    return true;
  }

  static uint32_t NEUI_ABI a_serialize_component(neui_session_t session,
                                                 neui_asset_t component,
                                                 char* out_buf, uint32_t cap,
                                                 int indent)
  {
    auto* s = get_session(session);
    if (!s || component.id == asset_none.id) return 0;
    if (((component.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return 0;
    auto* e = s->_asset_manager.get_slot(component.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_COMPONENT) return 0;
    neui_detail::ComponentSerializeInput in;
    in.name = &e->comp_name; in.width = e->comp_w; in.height = e->comp_h;
    in.params = &e->comp_params;
    in.asset_names = &e->comp_asset_names;
    in.asset_handle_names = &e->comp_asset_handle_names;
    in.asset_frame_layouts = &e->comp_asset_frame_layouts;
    auto* cce = s->_asset_manager.get_slot(e->comp_compound.id & 0xffff);
    auto* bbe = s->_asset_manager.get_slot(e->comp_behavior.id & 0xffff);
    in.compound = (cce && cce->compound) ? cce->compound.get() : nullptr;
    in.behavior = (bbe && bbe->behavior) ? bbe->behavior.get() : nullptr;
    std::string json = neui_detail::serialize_component(in, indent);
    uint32_t full = static_cast<uint32_t>(json.size());
    if (out_buf && cap > 0) {
      uint32_t n = (full > cap - 1) ? cap - 1 : full;
      if (n) std::memcpy(out_buf, json.data(), n);
      out_buf[n] = '\0';
    }
    return full;
  }

  static bool NEUI_ABI a_set_frame_layout(neui_session_t session,
                                          neui_asset_t asset,
                                          uint32_t cols, uint32_t rows,
                                          uint32_t gutter_px)
  {
    auto* s = get_session(session);
    if (!s || asset.id == asset_none.id) return false;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return false;
    return s->_asset_manager.set_frame_layout(asset.id & 0xffff,
                                              cols, rows, gutter_px);
  }

  static neui_asset_t NEUI_ABI a_create_filmstrip_from_file(
      neui_session_t session, const char* path_utf8,
      uint32_t frame_count, neui_filmstrip_orientation_t orientation)
  {
    auto* s = get_session(session);
    if (!s || !path_utf8) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_filmstrip_from_file(
        path_utf8, best_asset_scale_macos(), frame_count,
        orientation == NEUI_FILMSTRIP_HORIZONTAL,
        neui_cg_backend::get_backend());
    if (slot == 0) return asset_none;
    return pack_asset_macos(s->session_id(), slot);
  }

  static uint32_t NEUI_ABI a_get_frame_count(neui_session_t session,
                                             neui_asset_t asset)
  {
    auto* s = get_session(session);
    if (!s || asset.id == asset_none.id) return 0;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return 0;
    return s->_asset_manager.frame_count(asset.id & 0xffff);
  }

  neui_asset_api_t asset_api = {
    NEUI_VERSION,
    a_create_bitmap,
    a_create_from_file,
    a_destroy,
    a_get_size,
    a_get_kind,
    a_create_compound,
    a_create_behavior,
    a_create_surface,
    a_paint_surface,
    a_create_font,
    a_create_font_from_file,
    a_get_font_family,
    a_create_component_from_string,
    a_create_component_from_file,
    a_component_param_count,
    a_component_param_at,
    a_serialize_component,
    a_set_frame_layout,
    a_create_filmstrip_from_file,
    a_get_frame_count,
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

  static void NEUI_ABI co_set_path(neui_session_t session, neui_asset_t asset,
                                     neui_compound_layer_t layer,
                                     const neui_path_cmd_t* cmds,
                                     uint32_t count)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_macos(session, asset, layer, s);
    if (!L) return;
    neui_detail::apply_set_path(*L, cmds, count);
    s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_set_gradient(neui_session_t session, neui_asset_t asset,
                                       neui_compound_layer_t layer,
                                       const neui_gradient_t* grad)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_macos(session, asset, layer, s);
    if (!L) return;
    neui_detail::apply_set_gradient(*L, grad);
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
    co_set_path,
    co_set_gradient,
  };

  // Behavior API (NEUI_API_BEHAVIOR) - same shape as compound_api.
  // Mutations don't change paint output, so there's no invalidate walk;
  // per-write invalidate happens in the dispatch callbacks at run time.

  static neui_detail::BehaviorAsset*
  resolve_behavior_macos(neui_session_t session, neui_asset_t asset, Session*& out_session)
  {
    out_session = nullptr;
    auto* s = get_session(session);
    if (!s) return nullptr;
    if (asset.id == asset_none.id) return nullptr;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return nullptr;
    auto* e = s->_asset_manager.get_slot(asset.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_BEHAVIOR || !e->behavior) return nullptr;
    out_session = s;
    return e->behavior.get();
  }

  static neui_detail::BehaviorHandler*
  resolve_behavior_handler_macos(neui_session_t session, neui_asset_t asset,
                                   neui_behavior_handler_t handler, Session*& out_session)
  {
    out_session = nullptr;
    auto* s = get_session(session);
    if (!s) return nullptr;
    if (asset.id == asset_none.id) return nullptr;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return nullptr;
    uint32_t asset_slot = asset.id & 0xffff;
    if (neui_detail::behavior_handler_asset_slot(handler) != asset_slot) return nullptr;
    auto* e = s->_asset_manager.get_slot(asset_slot);
    if (!e || e->kind != NEUI_ASSET_KIND_BEHAVIOR || !e->behavior) return nullptr;
    out_session = s;
    return neui_detail::behavior_get_handler(*e->behavior,
                                              neui_detail::behavior_handler_slot(handler));
  }

  static neui_behavior_handler_t NEUI_ABI be_add_handler(neui_session_t session,
                                                          neui_asset_t asset,
                                                          neui_behavior_kind_t kind)
  {
    Session* s = nullptr;
    auto* ba = resolve_behavior_macos(session, asset, s);
    if (!ba) return behavior_handler_none;
    uint32_t asset_slot = asset.id & 0xffff;
    uint32_t slot = neui_detail::behavior_add_handler(*ba, kind);
    return neui_detail::pack_behavior_handler(asset_slot, slot);
  }

  static void NEUI_ABI be_remove_handler(neui_session_t session, neui_asset_t asset,
                                          neui_behavior_handler_t handler)
  {
    Session* s = nullptr;
    auto* ba = resolve_behavior_macos(session, asset, s);
    if (!ba) return;
    if (neui_detail::behavior_handler_asset_slot(handler) != (asset.id & 0xffff)) return;
    neui_detail::behavior_remove_handler(*ba, neui_detail::behavior_handler_slot(handler));
  }

  static void NEUI_ABI be_clear(neui_session_t session, neui_asset_t asset)
  {
    Session* s = nullptr;
    auto* ba = resolve_behavior_macos(session, asset, s);
    if (!ba) return;
    neui_detail::behavior_clear(*ba);
  }

  static void NEUI_ABI be_set_int(neui_session_t session, neui_asset_t asset,
                                    neui_behavior_handler_t handler,
                                    const char* prop, int value)
  {
    Session* s = nullptr;
    auto* H = resolve_behavior_handler_macos(session, asset, handler, s);
    if (!H || !prop) return;
    neui_detail::apply_behavior_set_int(*H, prop, value);
  }

  static void NEUI_ABI be_set_float(neui_session_t session, neui_asset_t asset,
                                      neui_behavior_handler_t handler,
                                      const char* prop, float value)
  {
    Session* s = nullptr;
    auto* H = resolve_behavior_handler_macos(session, asset, handler, s);
    if (!H || !prop) return;
    neui_detail::apply_behavior_set_float(*H, prop, value);
  }

  static void NEUI_ABI be_set_string(neui_session_t session, neui_asset_t asset,
                                       neui_behavior_handler_t handler,
                                       const char* prop, const char* value)
  {
    Session* s = nullptr;
    auto* H = resolve_behavior_handler_macos(session, asset, handler, s);
    if (!H || !prop) return;
    neui_detail::apply_behavior_set_string(*H, prop, value);
  }

  neui_behavior_api_t behavior_api = {
    NEUI_VERSION,
    be_add_handler,
    be_remove_handler,
    be_clear,
    be_set_int,
    be_set_float,
    be_set_string,
  };

  // -------------------------------------------------------------------------
  // Grid API (NEUI_API_GRID) - thin wrappers over WidgetData::grid_model.
  // Mechanical translation of hosts/win32/widgets.cpp::grid_api; the input +
  // paint glue lives in window.mm (the painted view). macOS coordinates are
  // logical points, so the viewport reads the view bounds directly (no DPI
  // conversion like win32's phys_to_log).

  static WidgetData* resolve_grid_macos(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return nullptr;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return nullptr;
    WidgetData* wd = &s->_widgets[idx];
    if (!wd->type || strcmp(wd->type, NEUI_W_GRID) != 0) return nullptr;
    return wd;
  }

  static neui_detail::GridModel& ensure_grid_model_macos_api(WidgetData& wd)
  {
    if (!wd.grid_model)
      wd.grid_model = std::make_unique<neui_detail::GridModel>();
    return *wd.grid_model;
  }

  static void grid_invalidate_macos(WidgetData* wd)
  {
    if (wd && wd->native_control)
      [(__bridge NSView*)wd->native_control setNeedsDisplay:YES];
  }

  // Viewport from the painted view's current bounds (logical points).
  static neui_detail::GridViewport grid_viewport_macos_api(WidgetData& wd)
  {
    auto& m   = ensure_grid_model_macos_api(wd);
    auto  cfg = neui_detail::grid_read_config(wd.attrs.get());
    int lw = 0, lh = 0;
    if (wd.native_control) {
      NSSize sz = ((__bridge NSView*)wd.native_control).bounds.size;
      lw = (int)sz.width;
      lh = (int)sz.height;
    }
    return neui_detail::grid_compute_viewport(m, lw, lh, cfg.row_h, cfg.header_h);
  }

  static int NEUI_ABI gr_add_column(neui_session_t session, neui_widget_t widget,
                                      const char* header, int width_logical)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd) return -1;
    auto& m = ensure_grid_model_macos_api(*wd);
    neui_detail::GridColumn c;
    c.header = header ? header : "";
    c.width  = (width_logical > 0) ? width_logical : neui_detail::GRID_DEFAULT_NEW_COLUMN_W;
    m.columns.push_back(std::move(c));
    neui_detail::grid_resize_rows_to_columns(m, (int)m.columns.size());
    grid_invalidate_macos(wd);
    return (int)m.columns.size() - 1;
  }

  static int NEUI_ABI gr_get_column_count(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_macos(session, widget);
    return wd && wd->grid_model ? (int)wd->grid_model->columns.size() : 0;
  }

  static void NEUI_ABI gr_set_column_width(neui_session_t session, neui_widget_t widget,
                                             int col, int width_logical)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return;
    auto cfg = neui_detail::grid_read_config(wd->attrs.get());
    int min_w = neui_detail::grid_column_min_width(m, col, cfg.col_min_w_def);
    if (width_logical < min_w) width_logical = min_w;
    m.columns[(size_t)col].width = width_logical;
    grid_invalidate_macos(wd);
  }

  static int NEUI_ABI gr_get_column_width(neui_session_t session, neui_widget_t widget, int col)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd || !wd->grid_model) return 0;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return 0;
    return m.columns[(size_t)col].width;
  }

  static void NEUI_ABI gr_set_column_min_width(neui_session_t session, neui_widget_t widget,
                                                  int col, int min_w)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return;
    m.columns[(size_t)col].min_width = min_w;
    if (m.columns[(size_t)col].width < min_w)
      m.columns[(size_t)col].width = min_w;
    grid_invalidate_macos(wd);
  }

  static void NEUI_ABI gr_set_column_align(neui_session_t session, neui_widget_t widget,
                                             int col, const char* align)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return;
    m.columns[(size_t)col].align = neui_detail::grid_parse_align(align);
    grid_invalidate_macos(wd);
  }

  static void NEUI_ABI gr_set_column_header(neui_session_t session, neui_widget_t widget,
                                              int col, const char* text)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return;
    m.columns[(size_t)col].header = text ? text : "";
    grid_invalidate_macos(wd);
  }

  static int NEUI_ABI gr_get_column_header(neui_session_t session, neui_widget_t widget,
                                             int col, char* buf, int buflen)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd || !wd->grid_model) return -1;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return -1;
    const std::string& h = m.columns[(size_t)col].header;
    int need = (int)h.size() + 1;
    if (buf && buflen > 0) {
      int copy = (need < buflen) ? need : buflen;
      memcpy(buf, h.c_str(), (size_t)copy);
      buf[copy - 1] = 0;
    }
    return need;
  }

  static void NEUI_ABI gr_remove_column(neui_session_t session, neui_widget_t widget, int col)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return;
    m.columns.erase(m.columns.begin() + col);
    for (auto& r : m.rows)
      if (col < (int)r.cells.size()) r.cells.erase(r.cells.begin() + col);
    std::unordered_map<uint64_t, neui_detail::GridCellOverride> remap;
    for (auto& kv : m.cell_overrides) {
      int r = (int)(kv.first >> 32);
      int c = (int)(kv.first & 0xFFFFFFFF);
      if (c == col) continue;
      int nc = (c > col) ? c - 1 : c;
      remap[neui_detail::grid_cell_key(r, nc)] = kv.second;
    }
    m.cell_overrides = std::move(remap);
    if (m.selected_col >= (int)m.columns.size())
      m.selected_col = (int)m.columns.size() - 1;
    neui_detail::grid_sort_on_column_removed(m, col);
    grid_invalidate_macos(wd);
  }

  static void NEUI_ABI gr_clear_columns(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_macos_api(*wd);
    m.columns.clear();
    m.rows.clear();
    m.cell_overrides.clear();
    m.selected_row = -1;
    m.selected_col = -1;
    m.scroll_offset_x = 0;
    m.scroll_offset_y = 0;
    m.scroll_px_offset = 0;
    m.sort_stack.clear();
    m.display_order.clear();
    m.logical_to_visual.clear();
    m.sort_dirty = false;
    grid_invalidate_macos(wd);
  }

  static int NEUI_ABI gr_add_row(neui_session_t session, neui_widget_t widget,
                                   const char* const* values_utf8)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd) return -1;
    auto& m = ensure_grid_model_macos_api(*wd);
    neui_detail::GridRow row;
    row.cells.resize(m.columns.size());
    if (values_utf8) {
      for (size_t i = 0; i < m.columns.size() && values_utf8[i]; ++i)
        row.cells[i] = values_utf8[i];
    }
    m.rows.push_back(std::move(row));
    m.sort_dirty = true;
    grid_invalidate_macos(wd);
    return (int)m.rows.size() - 1;
  }

  static int NEUI_ABI gr_get_row_count(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_macos(session, widget);
    return wd && wd->grid_model ? (int)wd->grid_model->rows.size() : 0;
  }

  static void NEUI_ABI gr_remove_row(neui_session_t session, neui_widget_t widget, int row)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (row < 0 || row >= (int)m.rows.size()) return;
    m.rows.erase(m.rows.begin() + row);
    std::unordered_map<uint64_t, neui_detail::GridCellOverride> remap;
    for (auto& kv : m.cell_overrides) {
      int r = (int)(kv.first >> 32);
      int c = (int)(kv.first & 0xFFFFFFFF);
      if (r == row) continue;
      int nr = (r > row) ? r - 1 : r;
      remap[neui_detail::grid_cell_key(nr, c)] = kv.second;
    }
    m.cell_overrides = std::move(remap);
    if (m.selected_row >= (int)m.rows.size())
      m.selected_row = (int)m.rows.size() - 1;
    m.sort_dirty = true;
    grid_invalidate_macos(wd);
  }

  static void NEUI_ABI gr_clear_rows(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    m.rows.clear();
    m.cell_overrides.clear();
    m.selected_row = -1;
    m.scroll_offset_y = 0;
    m.scroll_px_offset = 0;
    m.display_order.clear();
    m.logical_to_visual.clear();
    m.sort_dirty = false;
    grid_invalidate_macos(wd);
  }

  static void NEUI_ABI gr_set_cell_text(neui_session_t session, neui_widget_t widget,
                                          int row, int col, const char* utf8)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (row < 0 || row >= (int)m.rows.size()) return;
    if (col < 0 || col >= (int)m.columns.size()) return;
    auto& r = m.rows[(size_t)row];
    if ((int)r.cells.size() <= col) r.cells.resize((size_t)col + 1);
    r.cells[(size_t)col] = utf8 ? utf8 : "";
    m.sort_dirty = true;
    grid_invalidate_macos(wd);
  }

  static int NEUI_ABI gr_get_cell_text(neui_session_t session, neui_widget_t widget,
                                         int row, int col, char* buf, int buflen)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd || !wd->grid_model) return -1;
    auto& m = *wd->grid_model;
    if (row < 0 || row >= (int)m.rows.size()) return -1;
    if (col < 0 || col >= (int)m.columns.size()) return -1;
    const auto& r = m.rows[(size_t)row];
    static const std::string empty;
    const std::string& src = (col < (int)r.cells.size()) ? r.cells[(size_t)col] : empty;
    int need = (int)src.size() + 1;
    if (buf && buflen > 0) {
      int copy = (need < buflen) ? need : buflen;
      memcpy(buf, src.c_str(), (size_t)copy);
      buf[copy - 1] = 0;
    }
    return need;
  }

  static void NEUI_ABI gr_set_cell_color(neui_session_t session, neui_widget_t widget,
                                           int row, int col, uint32_t argb)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (row < 0 || row >= (int)m.rows.size()) return;
    if (col < 0 || col >= (int)m.columns.size()) return;
    if (argb == 0) {
      auto* ov = neui_detail::grid_find_override(m, row, col);
      if (ov) {
        ov->has_color = false;
        ov->color     = 0;
        neui_detail::grid_prune_override(m, row, col);
      }
    } else {
      auto& ov = neui_detail::grid_ensure_override(m, row, col);
      ov.color     = argb;
      ov.has_color = true;
    }
    grid_invalidate_macos(wd);
  }

  static void NEUI_ABI gr_set_cell_enabled(neui_session_t session, neui_widget_t widget,
                                             int row, int col, bool enabled)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (row < 0 || row >= (int)m.rows.size()) return;
    if (col < 0 || col >= (int)m.columns.size()) return;
    auto& ov = neui_detail::grid_ensure_override(m, row, col);
    ov.enabled     = enabled;
    ov.has_enabled = true;
    if (enabled && !ov.has_color) {
      ov.has_enabled = false;
      neui_detail::grid_prune_override(m, row, col);
    }
    grid_invalidate_macos(wd);
  }

  static void NEUI_ABI gr_clear_cell_overrides(neui_session_t session, neui_widget_t widget,
                                                  int row, int col)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd || !wd->grid_model) return;
    wd->grid_model->cell_overrides.erase(neui_detail::grid_cell_key(row, col));
    grid_invalidate_macos(wd);
  }

  static void NEUI_ABI gr_set_selected_row(neui_session_t session, neui_widget_t widget, int row)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_macos_api(*wd);
    int n = (int)m.rows.size();
    if (row < -1)  row = -1;
    if (row >= n)  row = n - 1;
    m.selected_row = row;
    auto cfg = neui_detail::grid_read_config(wd->attrs.get());
    if (cfg.cell_focus && m.selected_col < 0 && !m.columns.empty())
      m.selected_col = 0;
    if (row >= 0) {
      m.scroll_px_offset = 0;   // programmatic selection snaps to row alignment
      auto vp = grid_viewport_macos_api(*wd);
      neui_detail::grid_ensure_row_visible(m, vp, cfg.row_h, row);
    }
    grid_invalidate_macos(wd);
  }

  static int NEUI_ABI gr_get_selected_row(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_macos(session, widget);
    return wd && wd->grid_model ? wd->grid_model->selected_row : -1;
  }

  static void NEUI_ABI gr_set_selected_cell(neui_session_t session, neui_widget_t widget,
                                              int row, int col)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_macos_api(*wd);
    int n_rows = (int)m.rows.size();
    int n_cols = (int)m.columns.size();
    if (row < -1)       row = -1;
    if (row >= n_rows)  row = n_rows - 1;
    if (col < -1)       col = -1;
    if (col >= n_cols)  col = n_cols - 1;
    m.selected_row = row;
    m.selected_col = col;
    if (row >= 0 && col >= 0) {
      m.scroll_px_offset = 0;   // programmatic selection snaps to row alignment
      auto cfg = neui_detail::grid_read_config(wd->attrs.get());
      auto vp  = grid_viewport_macos_api(*wd);
      neui_detail::grid_ensure_cell_visible(m, vp, cfg.row_h, row, col);
    }
    grid_invalidate_macos(wd);
  }

  static void NEUI_ABI gr_get_selected_cell(neui_session_t session, neui_widget_t widget,
                                              int* out_row, int* out_col)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (out_row) *out_row = (wd && wd->grid_model) ? wd->grid_model->selected_row : -1;
    if (out_col) {
      if (!wd || !wd->grid_model) { *out_col = -1; return; }
      auto cfg = neui_detail::grid_read_config(wd->attrs.get());
      *out_col = cfg.cell_focus ? wd->grid_model->selected_col : -1;
    }
  }

  static void NEUI_ABI gr_ensure_row_visible(neui_session_t session, neui_widget_t widget, int row)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_macos_api(*wd);
    m.scroll_px_offset = 0;   // snap to row alignment
    auto cfg = neui_detail::grid_read_config(wd->attrs.get());
    auto vp  = grid_viewport_macos_api(*wd);
    neui_detail::grid_ensure_row_visible(m, vp, cfg.row_h, row);
    grid_invalidate_macos(wd);
  }

  static void NEUI_ABI gr_ensure_cell_visible(neui_session_t session, neui_widget_t widget,
                                                int row, int col)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_macos_api(*wd);
    m.scroll_px_offset = 0;   // snap to row alignment
    auto cfg = neui_detail::grid_read_config(wd->attrs.get());
    auto vp  = grid_viewport_macos_api(*wd);
    neui_detail::grid_ensure_cell_visible(m, vp, cfg.row_h, row, col);
    grid_invalidate_macos(wd);
  }

  static void NEUI_ABI gr_set_scroll_x(neui_session_t session, neui_widget_t widget, int x)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_macos_api(*wd);
    m.scroll_offset_x = x;
    auto cfg = neui_detail::grid_read_config(wd->attrs.get());
    auto vp  = grid_viewport_macos_api(*wd);
    neui_detail::grid_clamp_scroll(m, vp, cfg.row_h);
    grid_invalidate_macos(wd);
  }

  static int NEUI_ABI gr_get_scroll_x(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_macos(session, widget);
    return wd && wd->grid_model ? wd->grid_model->scroll_offset_x : 0;
  }

  static int NEUI_ABI gr_hit_test(neui_session_t session, neui_widget_t widget,
                                    int lx, int ly, int* out_row, int* out_col)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (out_row) *out_row = -1;
    if (out_col) *out_col = -1;
    if (!wd || !wd->grid_model) return 0;
    auto& m = *wd->grid_model;
    auto cfg = neui_detail::grid_read_config(wd->attrs.get());
    auto vp  = grid_viewport_macos_api(*wd);
    int widget_w = 0, widget_h = 0;
    if (wd->native_control) {
      NSSize sz = ((__bridge NSView*)wd->native_control).bounds.size;
      widget_w = (int)sz.width;
      widget_h = (int)sz.height;
    }
    neui_detail::grid_ensure_sort_clean(m);
    auto hit = neui_detail::grid_hit_test(m, vp, cfg.row_h,
                                           widget_w, widget_h, lx, ly);
    if (hit.region != neui_detail::GridHitRegion::Cell) return 0;
    if (out_row) *out_row = hit.row;
    if (out_col) *out_col = hit.col;
    return 1;
  }

  // -------- Sort API ----------------------------------------------------

  static void NEUI_ABI gr_set_column_sortable(neui_session_t session, neui_widget_t widget,
                                                int col, bool sortable)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return;
    m.columns[(size_t)col].sortable = sortable;
  }

  static void NEUI_ABI gr_set_column_sort_kind(neui_session_t session, neui_widget_t widget,
                                                 int col, neui_grid_sort_kind_t kind)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return;
    m.columns[(size_t)col].sort_kind = kind;
    if (neui_detail::grid_sort_stack_find(m, col) >= 0) {
      m.sort_dirty = true;
      grid_invalidate_macos(wd);
    }
  }

  static void NEUI_ABI gr_set_sort(neui_session_t session, neui_widget_t widget,
                                     int col, neui_grid_sort_dir_t dir)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_macos_api(*wd);
    neui_detail::grid_set_sort(m, col, dir);
    grid_invalidate_macos(wd);
  }

  static void NEUI_ABI gr_add_sort(neui_session_t session, neui_widget_t widget,
                                     int col, neui_grid_sort_dir_t dir)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_macos_api(*wd);
    neui_detail::grid_add_sort(m, col, dir);
    grid_invalidate_macos(wd);
  }

  static void NEUI_ABI gr_clear_sort(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_macos_api(*wd);
    neui_detail::grid_clear_sort(m);
    grid_invalidate_macos(wd);
  }

  static int NEUI_ABI gr_get_sort_count(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_macos(session, widget);
    return (wd && wd->grid_model) ? (int)wd->grid_model->sort_stack.size() : 0;
  }

  static void NEUI_ABI gr_get_sort_level(neui_session_t session, neui_widget_t widget,
                                           int level, int* out_col,
                                           neui_grid_sort_dir_t* out_dir)
  {
    if (out_col) *out_col = -1;
    if (out_dir) *out_dir = NEUI_GRID_SORT_NONE;
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (level < 0 || level >= (int)m.sort_stack.size()) return;
    if (out_col) *out_col = m.sort_stack[(size_t)level].col;
    if (out_dir) *out_dir = m.sort_stack[(size_t)level].dir;
  }

  static int NEUI_ABI gr_logical_to_visual_row(neui_session_t session, neui_widget_t widget,
                                                  int logical_row)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd || !wd->grid_model) return -1;
    auto& m = *wd->grid_model;
    if (logical_row < 0 || logical_row >= (int)m.rows.size()) return -1;
    neui_detail::grid_ensure_sort_clean(m);
    return neui_detail::grid_logical_to_visual(m, logical_row);
  }

  static int NEUI_ABI gr_visual_to_logical_row(neui_session_t session, neui_widget_t widget,
                                                  int visual_row)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd || !wd->grid_model) return -1;
    auto& m = *wd->grid_model;
    if (visual_row < 0 || visual_row >= (int)m.rows.size()) return -1;
    neui_detail::grid_ensure_sort_clean(m);
    return neui_detail::grid_visual_to_logical(m, visual_row);
  }

  // -------- Cell editing API (macOS) ------------------------------------
  // Edit dispatch helpers live in window.mm (next to grid_painted_msg_macos).
  // Forward declare them here so this TU can call into them.
  bool grid_try_begin_edit_macos(WidgetData& wd, int row, int col);
  bool grid_commit_edit_macos(WidgetData& wd);
  void grid_cancel_edit_macos(WidgetData& wd);

  static void NEUI_ABI gr_set_column_editable(neui_session_t session, neui_widget_t widget,
                                                int col, bool editable)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_macos_api(*wd);
    if (col < 0 || col >= (int)m.columns.size()) return;
    m.columns[(size_t)col].editable = editable;
    if (!editable && m.edit.active && m.edit.col == col)
      grid_cancel_edit_macos(*wd);
  }

  static bool NEUI_ABI gr_get_column_editable(neui_session_t session, neui_widget_t widget,
                                                int col)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd || !wd->grid_model) return false;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return false;
    return m.columns[(size_t)col].editable;
  }

  static void NEUI_ABI gr_begin_cell_edit(neui_session_t session, neui_widget_t widget,
                                           int row, int col)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd) return;
    if (grid_try_begin_edit_macos(*wd, row, col) && wd->native_control) {
      // Pull keyboard focus to the painted view so typing routes here.
      NSView* v = (__bridge NSView*)wd->native_control;
      [v.window makeFirstResponder:v];
    }
  }

  static void NEUI_ABI gr_end_cell_edit(neui_session_t session, neui_widget_t widget,
                                         bool commit)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd || !wd->grid_model || !wd->grid_model->edit.active) return;
    if (commit) (void)grid_commit_edit_macos(*wd);
    else        grid_cancel_edit_macos(*wd);
  }

  static bool NEUI_ABI gr_is_editing_cell(neui_session_t session, neui_widget_t widget,
                                            int* out_row, int* out_col)
  {
    auto* wd = resolve_grid_macos(session, widget);
    if (!wd || !wd->grid_model || !wd->grid_model->edit.active) {
      if (out_row) *out_row = -1;
      if (out_col) *out_col = -1;
      return false;
    }
    auto& m = *wd->grid_model;
    if (out_row) *out_row = m.edit.row;
    if (out_col) *out_col = m.edit.col;
    return true;
  }

  neui_grid_api_t grid_api = {
    NEUI_VERSION,
    gr_add_column,
    gr_get_column_count,
    gr_set_column_width,
    gr_get_column_width,
    gr_set_column_min_width,
    gr_set_column_align,
    gr_set_column_header,
    gr_get_column_header,
    gr_remove_column,
    gr_clear_columns,
    gr_add_row,
    gr_get_row_count,
    gr_remove_row,
    gr_clear_rows,
    gr_set_cell_text,
    gr_get_cell_text,
    gr_set_cell_color,
    gr_set_cell_enabled,
    gr_clear_cell_overrides,
    gr_set_selected_row,
    gr_get_selected_row,
    gr_set_selected_cell,
    gr_get_selected_cell,
    gr_ensure_row_visible,
    gr_ensure_cell_visible,
    gr_set_scroll_x,
    gr_get_scroll_x,
    gr_hit_test,
    gr_set_column_sortable,
    gr_set_column_sort_kind,
    gr_set_sort,
    gr_add_sort,
    gr_clear_sort,
    gr_get_sort_count,
    gr_get_sort_level,
    gr_logical_to_visual_row,
    gr_visual_to_logical_row,
    gr_set_column_editable,
    gr_get_column_editable,
    gr_begin_cell_edit,
    gr_end_cell_edit,
    gr_is_editing_cell,
  };

} // namespace macos_host
