#pragma once

#include <neui/neui.h>
#include "assets.h"
#include "events.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NEUI_API_WIDGETS "com.defiantnerd.neui.extension.widgets/0"

static const neui_widget_t widget_root = { 0 };
static const neui_widget_t widget_none = { UINT32_MAX };

typedef struct neui_widget_client {
  uint32_t neui_version;
  void (NEUI_ABI *ondestroy)(void* token, neui_widget_t widget, void* userdata);
  bool (NEUI_ABI *onevent)  (void* token, neui_event_t* event);
} neui_widget_client_t;

typedef struct neui_widget_api {
  // Create a widget under `parent`. (x, y) are logical pixels at 96 DPI,
  // relative to the parent's top-left (top-level children of a frame are
  // therefore frame-local). (width, height) are the widget's size in
  // logical pixels.
  //
  // For a top-level frame (APPWINDOW / PLUGWINDOW / DIALOG) (width, height)
  // is the CLIENT (content) area - the space your child widgets lay out in -
  // NOT the outer window including title bar / borders / menu. All hosts
  // honour this: a frame created at 720x480 gives a 720x480 usable client,
  // and get_client_rect / a child at (0,0)..(width,height) map into it.
  //
  // LAYOUT TIP (avoid edge clipping): a child is only fully visible when
  // x + width <= parent client width AND y + height <= parent client height.
  // Leave a margin (~8-12 px) between your widgets and the right / bottom
  // client edges rather than placing them flush at the edge - a widget at
  // x + width == client width has zero breathing room and reads as cramped
  // (and any rounding / scrollbar can clip it). When a frame may carry a
  // menubar, lay out against get_client_rect (which excludes an in-frame
  // menubar band) instead of the create() height.
  neui_widget_t      (NEUI_ABI *create)(neui_session_t session, neui_widget_t parent, const char* type, int x, int y, int width, int height, void* userdata);
  void               (NEUI_ABI *destroy)(neui_session_t session, neui_widget_t widget);
  void               (NEUI_ABI *show)(neui_session_t session, neui_widget_t widget);
  void               (NEUI_ABI *hide)(neui_session_t session, neui_widget_t widget);
  // (x, y) are parent-relative logical pixels, same convention as create().
  void               (NEUI_ABI *set_pos)(neui_session_t session, neui_widget_t widget, int x, int y, int width, int height);
  void               (NEUI_ABI *set_size)(neui_session_t session, neui_widget_t widget, int width, int height);
  void               (NEUI_ABI *set_emit_events)(neui_session_t session, neui_widget_t widget, bool enabled);
  void               (NEUI_ABI *set_text)(neui_session_t session, neui_widget_t widget, const char* text);
  // Returns bytes needed including null terminator (call with buf=NULL to query size).
  // Copies up to buflen bytes including null terminator if buf is non-NULL.
  // Returns -1 on error.
  int                (NEUI_ABI *get_text)(neui_session_t session, neui_widget_t widget, char* buf, int buflen);
  neui_widget_t      (NEUI_ABI *get_first_child)(neui_session_t session, neui_widget_t widget);
  neui_widget_t      (NEUI_ABI *get_next_sibling)(neui_session_t session, neui_widget_t widget);
  void               (NEUI_ABI *set_focus)(neui_session_t session, neui_widget_t widget);
  void               (NEUI_ABI *set_check)(neui_session_t session, neui_widget_t widget, neui_check_state_t state);
  neui_check_state_t (NEUI_ABI *get_check)(neui_session_t session, neui_widget_t widget);
  // Returns the platform-native window handle (e.g. HWND on Win32) for the widget,
  // or NULL if the widget has no native handle yet. Intended for NEUI_W_PLUGWINDOW
  // so the client can hand it to an external host for embedding.
  void*              (NEUI_ABI *get_native_handle)(neui_session_t session, neui_widget_t widget);
  // Mark the widget as a tab-stop so TAB / SHIFT+TAB keyboard traversal can move
  // focus to it. Has no effect on hosts that use native tab-order (e.g. Win32 host).
  void               (NEUI_ABI *set_tab_stop)(neui_session_t session, neui_widget_t widget, bool enabled);
  // Establish a modal owner for a dialog frame. While the dialog is shown,
  // the owner is input-blocked; on dialog destroy the owner is re-enabled
  // and re-activated. Must be called before show(dialog). Both widgets must
  // be frames; dialog is typically NEUI_W_DIALOG. Pass owner=widget_none to clear.
  void               (NEUI_ABI *set_owner)(neui_session_t session, neui_widget_t dialog, neui_widget_t owner);
  // Read back current widget geometry. Coordinates are logical pixels at
  // 96 DPI (matching set_pos / set_size / create). For frames the position
  // is relative to the screen / parent frame; for child widgets it is
  // relative to the parent widget's top-left (the same parent-relative
  // space create / set_pos use). Out-pointers may be NULL to ignore
  // individual fields. No-op if the widget does not exist.
  void               (NEUI_ABI *get_pos)(neui_session_t session, neui_widget_t widget, int* x, int* y);
  void               (NEUI_ABI *get_size)(neui_session_t session, neui_widget_t widget, int* width, int* height);
  // Show a popup menu anchored to a widget. (x, y) are in the anchor's
  // local logical coordinates; the host converts to screen. items is a
  // null-terminated array of UTF-8 strings; an item that equals "-" is a
  // separator. The call is BLOCKING - it returns when the user picks an
  // item or dismisses the menu. Returns the 1-based index of the picked
  // item (counting separators), or 0 if dismissed without picking.
  // Useful for context menus on KNOB / future widgets that need a
  // popup-style menu independent of the menubar.
  int                (NEUI_ABI *popup_menu)(neui_session_t session, neui_widget_t anchor,
                                             int x, int y,
                                             const char* const* items);
  // Request a repaint of `widget`. On NEUI_W_CUSTOMDRAW (and other painted
  // widgets) this re-fires the paint pass so the client can redraw with
  // updated state; on native widgets it asks the OS to repaint the control.
  // No-op if the widget has no native surface yet (before show()) or the
  // widget does not exist. Coalesces with any other pending invalidations
  // - one repaint per event-loop tick at most.
  void               (NEUI_ABI *invalidate)(neui_session_t session, neui_widget_t widget);
  // Bind a pre-loaded asset (see NEUI_API_ASSETS in <neui/d/assets.h>) as
  // the widget's source. NEUI_W_IMAGE is the only consumer today; on
  // other widget types this is a no-op.
  //
  // set_asset and set_text on an IMAGE are mutually clearing: the last
  // call wins and the previous source is released. Pass asset_none to
  // clear (paints empty). Cross-session handles are silently rejected.
  //
  // The widget does NOT retain a refcount on the asset. If the client
  // calls assets->destroy while a widget still references the handle,
  // subsequent paints resolve the slot to nullptr and paint as if
  // cleared - no crash, no draw. Recommended teardown: clear the
  // widget with set_asset(widget, asset_none) (or destroy the widget)
  // before assets->destroy.
  void               (NEUI_ABI *set_asset)(neui_session_t session, neui_widget_t widget, neui_asset_t asset);
  // Toggle the widget's enabled state. Disabled widgets paint dimmed and
  // do not receive input (mouse / focus). Default for every widget is
  // enabled=true. No-op if the widget does not exist. Frames + non-
  // interactive container widgets (SECTION, MENUBAR) accept the call
  // but the visual / focus effect is host-defined.
  // - win32 native host: calls EnableWindow on the underlying HWND; if
  //   the HWND has not been created yet (deferred), the flag is stored
  //   and applied at show().
  // - xpl host: stored as a flag on WidgetData; paint dims the widget
  //   (push_alpha(0.5) around its paint pass) and the hit-test / tab
  //   traversal skip disabled widgets.
  // - macOS native host: state is stored but the visual / input effect
  //   is not yet wired (TODO: NSControl setEnabled:).
  void               (NEUI_ABI *set_enabled)(neui_session_t session, neui_widget_t widget, bool enabled);
  bool               (NEUI_ABI *get_enabled)(neui_session_t session, neui_widget_t widget);
  // Read the usable content area of a widget in its own coordinate space
  // (logical px at 96 DPI). For a frame this is the area available for child
  // widgets, EXCLUDING any host-drawn in-frame menubar band: child widget
  // coordinates are relative to (x, y), and (width, height) is the content
  // size. This is the Win32 GetClientRect analogue - on platforms with a
  // native menu (Win32 HMENU / macOS NSMenu), a menubar-less frame, or a
  // non-frame widget, it returns (0, 0, widget_width, widget_height). Use it
  // instead of get_size when laying out against a frame that may carry a
  // menubar, so layout stays correct on the Linux in-frame-menubar host.
  // Out-pointers may be NULL to ignore fields. No-op if the widget does not
  // exist. (Vtable-appended; check the api version / pointer before calling.)
  void               (NEUI_ABI *get_client_rect)(neui_session_t session, neui_widget_t widget,
                                                  int* x, int* y, int* width, int* height);
  // Instantiate a CUSTOMDRAW widget from a COMPONENT asset (see
  // NEUI_ASSET_KIND_COMPONENT / neui_asset_api::create_component_*). Creates
  // the widget under `parent`, attaches the component's compound (visual) +
  // behavior (input), and stamps the component's default attrs. (x, y) are
  // parent-relative logical px; if width / height <= 0 the component's default
  // size is used. Set per-instance values afterwards via the attr API
  // (attrs->set_*). Returns widget_none on a bad / non-component asset or a
  // cross-session handle. (Vtable-appended; check the api version / pointer
  // before calling.)
  neui_widget_t      (NEUI_ABI *create_from_component)(neui_session_t session, neui_widget_t parent,
                                                       neui_asset_t component,
                                                       int x, int y, int width, int height);
  // Show a NEUI_W_POPUPMENU as a context menu anchored to a widget. Everything a
  // real context menu needs - submenus to arbitrary depth, per-item
  // enable/disable, checkmarks, shortcut LABELS, built-in command routing via
  // tree->set_menu_cmd, and NEUI_API_MENU_CLIENT::validate - is reachable,
  // because the menu is described with the same NEUI_API_TREE calls that build a
  // NEUI_W_MENUBAR. popup_menu's flat `const char* const*` can express none of
  // that; this is the rich alternative, not a replacement (popup_menu still
  // works everywhere and is unchanged).
  //
  // (x, y) are in the anchor's local logical coordinates, like popup_menu.
  //
  // ASYNCHRONOUS, unlike popup_menu, which blocks and returns the pick. This
  // call returns true as soon as the menu is on screen; the pick arrives later
  // as NEUI_EVENT_ITEM_SELECTED on the MENU widget, with .index carrying the
  // neui_item_t id that tree->add returned. Returns false without showing
  // anything for a non-POPUPMENU widget, a cross-session handle, an anchor with
  // no frame, or an EMPTY menu (an empty box that swallows the next click is
  // worse than doing nothing). Dismissal without a pick reports nothing at all.
  // A blocking variant would need a nested pump, which suppresses
  // NEUI_EVENT_TIMER for its duration and re-enters the client from inside its
  // own event handler.
  //
  // Exactly ONE ITEM_SELECTED per pick, and no NEUI_EVENT_TREE_ITEM_ACTIVATED
  // (which is the MENUBAR's pick event) - so a popup pick is never ambiguous
  // with a menu-bar pick.
  //
  // A row bound with tree->set_menu_cmd to a BUILT-IN command (< NEUI_CMD_USER_BASE,
  // e.g. NEUI_CMD_COPY) is offered to the focused widget first, exactly as the
  // menu bar does, so a "Copy" row on a context menu just works with no client
  // code. ITEM_SELECTED still fires afterwards either way. Binding a command in
  // the USER range has no effect here - handle those from ITEM_SELECTED, which
  // carries the item id you bound.
  //
  //   neui_widget_t m = widgets->create(sess, frame, NEUI_W_POPUPMENU, 0,0,0,0, NULL);
  //   neui_item_t cut  = tree->add(sess, m, tree_item_root, "Cut",  NULL);
  //   tree->set_menu_cmd(sess, m, cut, NEUI_CMD_CUT);
  //   tree->add(sess, m, tree_item_root, "-", NULL);          /* separator */
  //   neui_item_t more = tree->add(sess, m, tree_item_root, "More", NULL);
  //   tree->add(sess, m, more, "Nested", NULL);               /* submenu */
  //   widgets->popup_tree_menu(sess, knob, mx, my, m);        /* on RBUTTON_DOWN */
  //
  // Unlike a MENUBAR's, a POPUPMENU's per-item shortcut is a LABEL only - it is
  // drawn in the row, but nothing translates it into an accelerator. Bind the
  // real accelerator on the menu bar and use the label to show which key does
  // the same thing.
  //
  // Create the menu widget as a child of the frame (or of the anchor); it has no
  // size, position or visual presence of its own, and unlike NEUI_W_MENUBAR it
  // is NOT the frame's menu bar - a frame can carry both. Build it once and
  // re-show it, or rebuild per click with tree->clear.
  //
  // Unlike a MENUBAR's, a POPUPMENU's shortcut LABEL is drawn by neui itself, so
  // it uses the running platform's convention: "Ctrl+Shift+Z" on Windows/Linux,
  // the Cocoa glyphs on macOS ("^<shift-glyph><cmd-glyph>Z").
  //
  // XPL HOST ONLY, and only where that host routes input to the popup: win32,
  // macOS and Linux do; iOS does NOT (its touch path has no tree-popup
  // hit-test), so this returns false there rather than painting a menu that
  // cannot be picked or dismissed. The native win32 / macOS / iOS hosts leave
  // the slot NULL entirely - see the comment at each.
  // (Vtable-appended; check the api version / pointer before calling.)
  bool               (NEUI_ABI *popup_tree_menu)(neui_session_t session, neui_widget_t anchor,
                                                 int x, int y, neui_widget_t menu);
} neui_widget_api_t;

#define NEUI_W_APPWINDOW  "neui.std.appwindow"
#define NEUI_W_PLUGWINDOW "neui.std.plugwindow"
#define NEUI_W_DIALOG     "neui.std.dialog"
#define NEUI_W_LABEL      "neui.std.label"
#define NEUI_W_BUTTON     "neui.std.button"
#define NEUI_W_INPUTBOX   "neui.std.inputbox"
#define NEUI_W_CHECKBOX   "neui.std.checkbox"
#define NEUI_W_CHECKBOX3  "neui.std.checkbox3"
#define NEUI_W_LISTBOX    "neui.std.listbox"
#define NEUI_W_COMBOBOX   "neui.std.combobox"
#define NEUI_W_MULTILINE  "neui.std.multiline"
#define NEUI_W_TREEVIEW   "neui.std.treeview"
#define NEUI_W_MENUBAR    "neui.std.menubar"
// Model for widgets->popup_tree_menu. Built with the same NEUI_API_TREE calls as
// a MENUBAR, but it is NOT a menu bar: it is never attached to the frame, never
// reserves an in-frame band, and a frame may carry both.
#define NEUI_W_POPUPMENU  "neui.std.popupmenu"
#define NEUI_W_IMAGE      "neui.std.image"
#define NEUI_W_SLIDER     "neui.std.slider"
#define NEUI_W_KNOB       "neui.std.knob"
#define NEUI_W_SECTION    "neui.std.section"
#define NEUI_W_CUSTOMDRAW "neui.std.customdraw"
#define NEUI_W_GRID       "neui.std.grid"
#define NEUI_W_TABVIEW    "neui.std.tabview"
#define NEUI_W_TABPAGE    "neui.std.tabpage"

#ifdef __cplusplus
}
#endif