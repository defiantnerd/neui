#pragma once
#include <cstdint>
#include <neui/neui.h>

// enum neui_cursor_kind - the cursor shape set, shared with the native win32
// host and with the public NEUI_ATTR_CURSOR string attribute.
#include "cursor_kind.h"

// Forward-declarations for shared data-item types (defined in
// hosts/shared/clipboard_item.h). Decoupled here to keep platform.h light;
// implementation files include the full definition.
namespace neui_detail { class DataItem; }

// Platform abstraction for the crossplatform host.
// platform_win32.cpp implements this on Windows; platform_null.cpp elsewhere.

// Public NEUI_KEY_* / NEUI_KMOD_* constants come from <neui/neui.h>'s
// d/keys.h header. Mouse-modifier bits (NEUI_MK_*) come from d/events.h
// since they describe the public neui_event_mouse_t::buttonmap field.
//
// The NEUI_MK_* values were CHOSEN to match Win32 MK_*, but a platform must
// still MASK rather than forward a raw wParam: MK_XBUTTON1 is 0x0020, which is
// the bit NEUI_MK_ALT reserves, so forwarding would report a phantom Alt on
// any mouse with side buttons. Each OS has a translation helper for this -
// hosts/shared/{win32/keys_win32.h,macos/keys_macos.h,linux/keys_linux.h}
// (win32_buttonmap / mac_buttonmap / x11_buttonmap). Use them.

namespace xpl_host
{
  class Session;
  class WidgetData;

  // Initialize platform resources (window class registration, etc.).
  // Safe to call multiple times; no-op after the first call.
  void platform_init();

  // Returns the rendering backend for this platform.
  neui_render_backend_t* platform_get_backend();

  // Create a native top-level application window (APPWINDOW).
  // Sets wd.native_handle, wd.render_ctx, and wd.dpi on success.
  void platform_create_appwindow(Session* session, uint32_t widget_index,
                                  WidgetData& wd);

  // Create a borderless embeddable plugin window (PLUGWINDOW).
  // Sets wd.native_handle, wd.render_ctx, and wd.dpi on success.
  // If wd.embed_parent != 0 (see platform_set_embed_parent) the frame is
  // created inside that foreign native parent instead of as its own
  // top-level: Win32 = WS_CHILD of the parent HWND, macOS = an NEUIView
  // subview of the parent NSView (no NSWindow of its own), Linux = a child
  // of the foreign X11 Window over a dedicated Display connection with no
  // neui-owned event loop (the DAW drives platform_embed_pump_and_tick).
  void platform_create_plugwindow(Session* session, uint32_t widget_index,
                                   WidgetData& wd);

  // -------------------------------------------------------------------------
  // DAW-embedding seams - the neui side of a plugin adapter: a
  // foreign-parent child frame plus (where the platform needs one) a
  // host-driven pump. Implemented on every platform layer (null host stubs);
  // the actual VST3/CLAP/LV2 SDK glue is a separate, out-of-scope effort
  // that calls these - normally through the public NEUI_API_EMBED interface
  // (include/neui/d/embed.h), which forwards here.

  // Set the embed target for the next PLUGWINDOW created for this widget.
  // native_parent is the DAW-provided parent: HWND on Win32, NSView* on
  // macOS, the X11 Window id (cast through uintptr_t/void*) on Linux.
  // nullptr = standalone top-level. Must be called before widget_show.
  void platform_set_embed_parent(Session* session, uint32_t widget_index,
                                 void* native_parent);

  // A pollable event file descriptor for the embedded frame, or -1 when the
  // platform doesn't need one. Linux: the dedicated Display's X11 connection
  // fd - the DAW registers it with its run loop (VST3 IRunLoop
  // registerEventHandler / CLAP posix-fd) so it knows when to pump. Win32 /
  // macOS: -1 - the DAW's own message pump / main runloop already delivers
  // paint, input, and timers to a child HWND / NSView subview.
  int platform_embed_event_fd(void* native_handle);

  // Service the embedded frame from the DAW's periodic timer (VST3 IRunLoop
  // registerTimer / CLAP timer / LV2 idle) and whenever
  // platform_embed_event_fd signals readable. Linux: drains the dedicated
  // Display, advances at most one animation tick per ~16 ms, repaints - the
  // ONLY heartbeat in embedded mode. Win32 / macOS: a no-op kept for a
  // platform-uniform adapter loop (WM_PAINT / WM_TIMER / NSTimer already
  // arrive through the DAW's pump).
  void platform_embed_pump_and_tick(void* native_handle);

  // Create a dialog frame (DIALOG): titlebar + close button, no resize, no
  // minimize/maximize buttons, optionally owned by `owner_native` (its
  // input is blocked while the dialog is shown - see platform_set_window_enabled).
  // Does NOT count toward the appwindow quit-count. owner_native may be nullptr.
  void platform_create_dialog(Session* session, uint32_t widget_index,
                               WidgetData& wd, void* owner_native);

  // Destroy a native window and release its render context.
  void platform_destroy_window(WidgetData& wd);

  // Show / hide a native window.
  void platform_show_window(void* native_handle);
  void platform_hide_window(void* native_handle);

  // Enable / disable input on a native frame. Disabled frames stay visible
  // but ignore mouse and keyboard. Used for modal-dialog blocking.
  void platform_set_window_enabled(void* native_handle, bool enabled);

  // Bring a native frame to the front and activate it.
  void platform_activate_window(void* native_handle);

  // Set the title text of a native frame window (UTF-8).
  void platform_set_window_title(void* native_handle, const char* text);

  // Resize / reposition a native window. Coordinates are logical pixels (96 DPI base).
  // Sentinel for platform_set_window_pos's x / y: "leave the window where it
  // is, change only the size". Needed because no xpl platform tracks user
  // window moves back into wd.x/y, so passing the stored position would
  // teleport a window the user had dragged - which is what a live zoom change
  // would otherwise do on every adjustment.
  static constexpr int NEUI_WINDOW_POS_KEEP = (-2147483647 - 1);   // INT_MIN

  void platform_set_window_pos(void* native_handle,
                                int x, int y, int w, int h, uint32_t dpi);

  // Post an asynchronous close request to a native window.
  void platform_post_close(void* native_handle);

  // Invalidate the entire client area of a native window so it repaints on the
  // next message-loop iteration. Used after focus changes to update the outline.
  void platform_invalidate(void* native_handle);

  // Repaint a native window NOW, synchronously, before returning - unlike
  // platform_invalidate, which only marks it dirty for the next loop iteration.
  //
  // Exists for out-of-band positional queries (Session::ensure_abs_positions):
  // some layout state is only produced by the paint path, so a caller that must
  // answer a geometry question about a never-painted frame has to make that
  // paint happen rather than duplicate the computation. Do NOT reach for this to
  // "make a repaint happen sooner" - that is what platform_invalidate is for.
  //
  // Best-effort: a platform with no synchronous repaint entry point, or a window
  // that is hidden / off-screen / not yet mapped, may legitimately do nothing, in
  // which case the caller's cached geometry stays stale rather than becoming
  // wrong. Must not be called from inside a paint (it would re-enter).
  void platform_force_paint(void* native_handle);

  // Start / stop a 16 ms repaint heartbeat on a frame's native window.
  // Used by the toast overlay (and any future animation that does not
  // belong to a single widget). The platform layer is responsible for
  // calling platform_invalidate on each tick so the frame's paint pass
  // can advance the toast animation phase. Idempotent: starting twice
  // on the same handle resets the timer; stopping a non-running timer
  // is a silent no-op.
  void platform_start_toast_animation(void* native_handle);
  void platform_stop_toast_animation(void* native_handle);

  // Returns monotonic milliseconds since some fixed reference. Used by
  // the toast overlay to drive phase math independently of frame-rate.
  uint64_t platform_now_ms();

  // Modal message box owned by the given frame's native window. Mimics
  // Win32 MessageBoxEx: `flags` is a NEUI_MB_* combination (numerically
  // equal to MB_*). Blocks until the user picks a button; returns the
  // NEUI_ID_* of the chosen button, 0 on failure / null platform.
  // win32 = MessageBoxExW pass-through (neutral language id); macOS =
  // NSAlert run modally.
  int platform_message_box(void* native_handle, const char* text,
                           const char* caption, uint32_t flags);

  // Returns the scale factor (physical pixels per logical pixel) for a native
  // window handle: 1.0f at 96 DPI, 2.0f at 192 DPI, etc.
  float platform_get_scale_factor(void* native_handle);

  // Load an image file and decode it to BGRA8 premultiplied pixels.
  // Returns a heap-allocated pixel buffer (caller must call platform_free_image).
  // Returns nullptr on failure. On success, *width_out and *height_out are the
  // physical pixel dimensions of the decoded image.
  uint8_t* platform_load_image(const char* path,
                                uint32_t* width_out, uint32_t* height_out);
  void platform_free_image(uint8_t* pixels);

  // Run the platform message loop.
  // Blocks until all application windows are closed. Returns true on normal exit.
  bool platform_run();

  // Drain pending platform events without blocking. Returns false on quit.
  bool platform_pump_once();

  // Run a nested platform message loop until *keep_running is set false
  // (typically by a mouse / key handler invoked via DispatchMessage from
  // inside the loop). Used by widgets->popup_menu to block until the
  // user picks an item or dismisses. Returns false on quit (WM_QUIT seen),
  // true on normal exit via *keep_running.
  bool platform_run_modal_until(bool* keep_running);

  // -------------------------------------------------------------------------
  // System clipboard.
  //
  // Text-only fast-path delegates to hosts/shared/win32/clipboard_win32.h
  // and hosts/shared/macos/clipboard_macos.h. The item-based path handles
  // any MIME on the DataItem - text/plain, text/html, text/uri-list, plus
  // arbitrary custom MIMEs pass through as registered clipboard formats
  // (Win32) or pasteboard UTI types (macOS).
  //
  // The host's xpl widgets (InputBoxWidget, MultilineWidget) call the
  // text helpers directly for Ctrl+C/X/V hot paths, bypassing the public
  // clipboard API (which is also routed through these helpers).

  // Replace clipboard with utf8/length bytes. Returns true on success.
  bool platform_clipboard_set_text(const char* utf8, uint32_t length);

  // Read clipboard text into buf as UTF-8. Returns total bytes needed
  // including null terminator. buf=NULL queries size only. Returns 0 if
  // clipboard has no text. Returns -1 on error.
  int  platform_clipboard_get_text(char* buf, int buflen);

  // True if the clipboard currently advertises Unicode text.
  bool platform_clipboard_has_text();

  // X11 PRIMARY selection (select-to-copy / middle-click-paste). Linux only;
  // no-op / 0 on Win32 + macOS (no PRIMARY concept). set publishes the current
  // text selection so other apps (and our own middle-click) can paste it; get
  // reads whatever owns PRIMARY now (same return contract as get_text - total
  // bytes incl. NUL, buf=NULL queries size, 0 = none).
  void platform_clipboard_set_primary(const char* utf8, uint32_t length);
  int  platform_clipboard_get_primary(char* buf, int buflen);

  // Place every format on the item onto the system clipboard.
  // Returns true if at least one format was published.
  bool platform_clipboard_write_item(const neui_detail::DataItem& item);

  // Snapshot every representation on the system clipboard into the item.
  // Returns true if at least one format was captured.
  bool platform_clipboard_read_item(neui_detail::DataItem& item);

  // -------------------------------------------------------------------------
  // Drag & drop drop-target registration.
  //
  // Called by the host's widget_show on top-level frame windows. The
  // platform layer wraps the native handle as a drop target (Win32:
  // RegisterDragDrop with a synthesised IDropTarget COM object; macOS:
  // [contentView registerForDraggedTypes:...] + NSDraggingDestination
  // protocol methods on the content view). The platform layer holds
  // (session_ptr, frame_widget_id) and calls back into the Session via
  // Session::dispatch_dnd_* (one entry per OS callback) so the framework
  // can hit-test, fire client events, and report the accepted action
  // back to the OS.
  //
  // Returns true if registration succeeded. Returns false on platforms
  // without DnD support (the framework will silently swallow drops).

  bool platform_dnd_register_window(void* native_handle,
                                     void* session_ptr,
                                     uint32_t frame_widget_id);
  void platform_dnd_unregister_window(void* native_handle);

  // Initiate a synchronous OS-level drag from `native_handle` (frame HWND
  // on Win32; frame content NSView on macOS). Spins the OS drag loop /
  // a nested runloop until the user drops or cancels. `item` is borrowed
  // for the duration (caller still owns it - the platform layer
  // snapshots formats before the loop). Returns the negotiated
  // neui_dnd_action_t value; 0 on cancel.
  //
  // `preview_native` is an OS-specific pre-built drag-preview object
  // (HBITMAP on Win32, retained NSImage* on macOS, nullptr elsewhere or
  // when no preview was supplied). Ownership transfers to the platform
  // layer for the duration of the call (Win32 hands the HBITMAP to
  // IDragSourceHelper which deletes it; macOS releases the NSImage on
  // return from the call). Pass nullptr for "OS default visual".
  // `hot_x`/`hot_y` are the hot-spot on the preview image; -1 = centre.
  uint32_t platform_dnd_begin_drag(void* native_handle,
                                    neui_detail::DataItem* item,
                                    uint32_t allowed_actions,
                                    void* preview_native = nullptr,
                                    int hot_x = -1,
                                    int hot_y = -1);

  // Materialise a per-platform drag-preview bitmap object from raw BGRA8
  // (premultiplied) pixels. Returns nullptr on failure or on the null
  // platform. The returned object is opaque to the xpl layer: Win32
  // returns an HBITMAP (caller owns until handed to platform_dnd_begin_drag,
  // which transfers ownership to IDragSourceHelper on success); macOS
  // returns a retained NSImage* bridged to void*. Ownership is consumed
  // by the matching platform_dnd_begin_drag call.
  void* platform_make_drag_preview(const uint8_t* bgra_premul,
                                     uint32_t w_px, uint32_t h_px,
                                     float scale);

  // -------------------------------------------------------------------------
  // Native menu bar support.
  // All hmenu / parent_hmenu / submenu parameters are HMENU on Win32 and
  // opaque void* (always nullptr) on non-Windows platforms.

  // True when the platform has no native menu bar and the host must draw the
  // menubar itself inside the frame's client area (reserving a top band and
  // offsetting child widgets below it). Linux/X11 returns true; Win32 (HMENU)
  // and macOS (global NSMenu) return false, as does the null platform. The
  // shared paint walk consults this (via Session::frame_top_inset) so only the
  // in-frame platforms reserve band space.
  bool platform_menubar_in_frame();

  // Additive per-platform top inset (logical px), ADDED to whatever the
  // in-frame band painter reserves. This is the seam for platforms that must
  // keep content out from under system chrome WITHOUT drawing the Linux-style
  // cascading-dropdown band (so platform_menubar_in_frame() stays false there).
  //
  //   iOS: the status bar / notch safe area (view.safeAreaInsets.top) plus, when
  //        the frame carries a MENUBAR child, a band tall enough for the native
  //        hamburger UIButton that opens the menu as a UIMenu popover. The
  //        hamburger is a real subview (not painted by paint_menubar, which is
  //        gated off on iOS), so only the inset reservation is shared.
  //   Win32 / macOS / Linux / null: 0 (their menu chrome is native or already
  //        handled by the in-frame band). Returning 0 keeps frame_top_inset
  //        byte-for-byte unchanged on those platforms.
  //
  // frame_native_handle is the frame's native window/view handle (may be null
  // before the window is created); has_menubar is true when the frame has a
  // visible MENUBAR child.
  int  platform_frame_extra_top_inset(void* frame_native_handle, bool has_menubar);

  // Create a new top-level menu bar handle.
  // menubar_widget_id is the encoded (session_id<<16 | widget_index) used by
  // the macOS implementation to route NSMenuItem activations back to the
  // owning Session::dispatch_menu_event. Win32 ignores it (the centralised
  // HACCEL + WM_COMMAND already encode the routing).
  void* platform_menubar_create(uint32_t menubar_widget_id);

  // Destroy a menu bar and all its popup submenus.
  void  platform_menubar_destroy(void* hmenu);

  // Attach a menu bar to a native frame window and repaint it.
  void  platform_menubar_attach(void* frame_hwnd, void* hmenu);

  // Force the frame window's menu bar to repaint (call after any structural change).
  void  platform_menubar_refresh(void* frame_hwnd);

  // Append a top-level popup entry to the menu bar.
  // display_text is UTF-8. Returns the new popup HMENU (nullptr on failure).
  void* platform_menubar_add_popup(void* hmenu, const char* display_text);

  // Append a command item to a popup menu.
  // display_text is UTF-8 and already includes the tab+shortcut suffix if any.
  void  platform_menubar_add_item(void* parent_hmenu, uint32_t cmd_id,
                                   const char* display_text);

  // Append a separator to a popup menu.
  // cmd_id is stored so the separator can be removed later.
  void  platform_menubar_add_separator(void* parent_hmenu, uint32_t cmd_id);

  // Remove a popup submenu entry from its parent menu (and destroy the submenu).
  void  platform_menubar_remove_popup(void* parent_hmenu, void* submenu);

  // Remove a command item or separator (identified by cmd_id) from its parent menu.
  void  platform_menubar_remove_item(void* parent_hmenu, uint32_t cmd_id);

  // Enable or disable a command item.
  void  platform_menubar_enable_item(void* parent_hmenu, uint32_t cmd_id, bool enabled);

  // Enable or disable a top-level popup entry (identified by the submenu pointer).
  void  platform_menubar_enable_popup(void* parent_hmenu, void* submenu, bool enabled);

  // Show or hide a checkmark on a leaf command item.
  // Win32: CheckMenuItem(MF_CHECKED / MF_UNCHECKED).
  // macOS: NSMenuItem.state = NSControlStateValueOn / Off.
  // Linux: no-op (the in-frame menu reads MenuItemData::checked when painting).
  // iOS / null: no-op.
  void  platform_menubar_check_item(void* parent_hmenu, uint32_t cmd_id, bool checked);

  // Replace the display text of an existing command item (text already includes
  // any tab+shortcut suffix).
  void  platform_menubar_set_item_text(void* parent_hmenu, uint32_t cmd_id,
                                        const char* display_text);

  // -------------------------------------------------------------------------
  // Frame-level "polish" surfaces.
  //
  // These wrap small platform divergences in window-level state so the host
  // doesn't need #ifdef _WIN32 / #else / #endif blocks at every attr-setter.

  // Apply an app/window icon from a UTF-8 file path.
  // Win32: per-window via WM_SETICON (small + big).
  // macOS: process-wide via NSApp.applicationIconImage (which is also what
  //        appears in the Dock - there's no per-window icon concept).
  // Null:  no-op.
  void platform_set_window_icon(WidgetData& wd, const char* path_utf8);

  // Apply min/max content-size constraints (logical pixels at 96 DPI base) to
  // a frame window. min_w/min_h/max_w/max_h <= 0 mean "no constraint" for that
  // dimension.
  // Win32: stored on the widget; WM_GETMINMAXINFO reads attrs at runtime, so
  //        this shim is a no-op (the existing pull path handles it).
  // macOS: setContentMinSize: / setContentMaxSize: on the NSWindow.
  void platform_apply_size_constraints(void* native_handle,
                                        int min_w, int min_h,
                                        int max_w, int max_h);

  // Apply a typed shortcut to a menu item.
  // Win32: no-op (the menubar's HACCEL is rebuilt centrally and the
  //        accelerator-table translation drives WM_COMMAND).
  // macOS: sets NSMenuItem.keyEquivalent + keyEquivalentModifierMask.
  // Pass key=NEUI_KEY_NONE to clear.
  void  platform_menubar_set_item_shortcut(void* parent_hmenu, uint32_t cmd_id,
                                            uint32_t modifiers, uint32_t key);

  // -------------------------------------------------------------------------
  // Mouse cursor.
  //
  // The shape set is `enum neui_cursor_kind` in hosts/shared/cursor_kind.h,
  // shared with the native win32 host (which used to keep its own copy of the
  // list) and with the public NEUI_ATTR_CURSOR string attribute.
  //
  // Called on every hover change and on the internal transient overrides
  // (the GRID header column-resize divider), so it must be cheap and
  // idempotent - Session::refresh_cursor already suppresses no-op changes,
  // but a platform that round-trips to a server should dedupe as well.
  //
  // STICKINESS IS THE PLATFORM'S JOB, and it is not automatic: on win32 the
  // window class registers an arrow in wc.hCursor and DefWindowProc reapplies
  // it on every WM_SETCURSOR, and on macOS AppKit resets the cursor from the
  // view's cursor rects on every mouse-moved. A platform must therefore track
  // the active kind and reapply it from its cursor-update message
  // (WM_SETCURSOR / -cursorUpdate:), not merely set it once here.
  //
  // NEUI_CURSOR_DEFAULT reaches the platform already resolved to "no widget
  // asked for anything", so it means the OS arrow. Inheritance up the widget
  // tree is resolved in Session::resolve_cursor_for before this is called.
  void platform_set_cursor(int kind /* neui_cursor_kind */);

  // -------------------------------------------------------------------------
  // Relative (unbounded) pointer mode (NEUI_API_POINTER, <neui/d/pointer.h>).
  //
  // Pins the visible cursor while a drag keeps consuming device motion, so a
  // knob drag doesn't die at the screen edge. Session owns the virtual-position
  // bookkeeping (hosts/shared/relative_pointer.h); these three calls are only
  // the platform mechanics.
  //
  // platform_begin_relative_pointer records the anchor in whatever coordinates
  // that platform needs for the warp-back (SCREEN px on every current
  // platform - deliberately opaque to the caller, which only passes them back)
  // and returns false when the platform has no seam at all (iOS / null), so
  // begin_relative can honestly report failure instead of pinning nothing.
  //
  // The three implementations are NOT the same shape, and the difference is
  // load-bearing:
  //   - macOS uses CGAssociateMouseAndMouseCursorPosition(false), which
  //     decouples the cursor from the device WITHOUT moving it. No per-move
  //     warp, and therefore no synthetic motion event to filter.
  //   - win32 (SetCursorPos) and X11 (XWarpPointer) must warp back on every
  //     move, and each warp generates a fresh motion event that has to be
  //     recognised and dropped - see relative_is_warp_echo. A handler that
  //     misses this reads the echo as an equal-and-opposite delta and the
  //     pointer appears frozen.
  bool platform_supports_relative_pointer();
  bool platform_begin_relative_pointer(void* native_handle,
                                        int* out_anchor_x, int* out_anchor_y);
  void platform_end_relative_pointer(void* native_handle,
                                      int anchor_x, int anchor_y);

  // -------------------------------------------------------------------------
  // Client timers (NEUI_API_TIMER, <neui/d/timer.h>).
  //
  // ONE native periodic tick per session, not one per client timer: the
  // portable TimerTable (hosts/shared/timer_table.h) owns deadlines and
  // multiplexes, so the platform only has to fire session->tick_client_timers()
  // every `interval_ms`. Start is idempotent and re-arms at a new interval when
  // the shortest live timer changes; stop is called as soon as the last timer
  // goes away so an idle session burns no wakeups.
  //
  // Must work under run(), under a hand-rolled pump_once() loop, and under
  // NEUI_API_EMBED - i.e. it has to hang off whatever already services the
  // session, not off a thread of its own.
  // True when this platform layer implements the INPUT half of
  // NEUI_ATTR_UI_SCALE: dividing mouse / touch coordinates by the zoom and
  // scaling the native window. The paint-side CTM in Session::paint_frame is
  // platform-independent, so a platform that scales paint without dividing
  // input would hit-test in a different space than it draws - which is worse
  // than not zooming. WidgetData::ui_scale() returns 1.0 when this is false,
  // making the attr inert rather than broken.
  bool platform_supports_ui_scale();

  // True when this platform layer routes input to the standalone tree popup
  // (widgets->popup_tree_menu): press / move / release plus Esc-to-dismiss, and
  // draining Session::tree_popup_take_release.
  //
  // Session::paint_frame paints the cascade platform-independently, so a
  // platform that paints it without wiring input would put up a menu that the
  // user can neither pick from nor dismiss, and whose taps fall THROUGH to the
  // widgets underneath - strictly worse than not offering the feature.
  // show_tree_popup therefore refuses when this is false, so the API fails
  // honestly instead of painting something inert. (Same shape as
  // platform_supports_ui_scale, and as begin_relative on iOS / null.)
  bool platform_supports_tree_popup();

  // Modal file dialog owned by the given frame's native window, behind
  // NEUI_API_NOTIFY::open_file / save_file. `save` selects the save flavour
  // (one destination, extension completion, overwrite prompt) over the open
  // one (existing file(s) or a folder). Blocks until the user confirms or
  // cancels - the same contract as platform_message_box - and invokes `cb`
  // once per chosen path before returning.
  //
  // Returns the number of paths delivered: 0 = cancelled, N>0 = that many
  // callbacks fired, -1 = this platform has no file-dialog surface (null,
  // iOS). Unlike platform_message_box, -1 and 0 are distinct: the public API
  // promises a client can tell "no dialog" from "user said no".
  //
  // A NULL `cb` is legal and still runs the dialog (the count is the useful
  // part); implementations must not dereference it.
  int platform_file_dialog(void* native_handle, int save,
                           const neui_file_dialog_t* desc,
                           neui_file_path_cb cb, void* userdata);

  void platform_timer_start(Session* session, uint32_t interval_ms);
  void platform_timer_stop(Session* session);

  // -------------------------------------------------------------------------
  // Accessibility (NEUI_API_A11Y, <neui/d/a11y.h>).
  //
  // The TREE itself is not a seam: a11y_adapter.cpp + hosts/shared/a11y_tree.h
  // build it once, portably, for every platform (the xpl host paints one native
  // surface per frame, so the tree has to be synthesised whatever the platform
  // provider API looks like). What a platform layer adds is the PROVIDER - the
  // object the OS asks - plus the two things a provider cannot pull:
  // notifications, and a spoken announcement with no node behind it.
  //
  // Both take the FRAME's native handle, because a provider is rooted at one
  // frame's native surface (per-frame focus, per-frame subtree - see G3 in
  // plans/accessibility.md). A platform with no provider implements these as
  // no-ops and the accessibility API stays honest: declarations are stored and
  // cost nothing, and nothing reads them. That is Linux, iOS and null today.

  // What changed, for platform_a11y_notify. The first five mirror
  // neui_a11y_change_t exactly, so a client's notify() forwards its argument
  // unchanged. Focus has no public counterpart - a client never drives focus by
  // hand - so it sits above the public range rather than renumbering it.
  enum A11yNotifyKind
  {
    a11y_notify_value     = 0,   // == NEUI_A11Y_CHANGE_VALUE
    a11y_notify_name      = 1,
    a11y_notify_state     = 2,
    a11y_notify_structure = 3,
    a11y_notify_selection = 4,
    a11y_notify_focus     = 100
  };

  // `widget_id` is the PUBLIC widget id (session << 16 | slot), not a tree
  // slot, so this seam needs no Session. A provider that has not been asked for
  // anything yet should do nothing: posting a notification for a tree it has
  // never built would make the OS query at an arbitrary moment for no reason.
  void platform_a11y_notify(void* frame_native_handle, uint32_t widget_id,
                            int change /* A11yNotifyKind */);

  // Speak `utf8` now, with no node behind it - the accessibility counterpart of
  // a toast. `assertive` interrupts whatever is being spoken.
  void platform_a11y_announce(void* frame_native_handle, const char* utf8,
                              bool assertive);

  // "Is an AT actually listening right now", when the platform can say. Backs
  // half of NEUI_API_A11Y::is_active, which is documented as ADVISORY - a client
  // must never gate correctness on it.
  //
  // Only win32 can answer honestly (UiaClientsAreListening). macOS has no such
  // signal by design - accessibility there is lazy, so "has anything queried us"
  // is the only truthful answer and that is what the Session flag tracks. A
  // platform with no provider returns false, which is not conservative but exact.
  bool platform_a11y_is_listening();

} // namespace xpl_host
