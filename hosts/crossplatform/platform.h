#pragma once
#include <cstdint>
#include <neui/neui.h>

// Forward-declarations for shared data-item types (defined in
// hosts/shared/clipboard_item.h). Decoupled here to keep platform.h light;
// implementation files include the full definition.
namespace neui_detail { class DataItem; }

// Platform abstraction for the crossplatform host.
// platform_win32.cpp implements this on Windows; platform_null.cpp elsewhere.

// Public NEUI_KEY_* / NEUI_KMOD_* constants come from <neui/neui.h>'s
// d/keys.h header. Mouse-modifier bits (NEUI_MK_*) come from d/events.h
// since they describe the public neui_event_mouse_t::buttonmap field;
// numeric values match Win32 MK_* so the platform layer forwards wParam
// directly.

namespace xpl_host
{
  class Session;
  struct WidgetData;

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
  void platform_create_plugwindow(Session* session, uint32_t widget_index,
                                   WidgetData& wd);

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
  void platform_set_window_pos(void* native_handle,
                                int x, int y, int w, int h, uint32_t dpi);

  // Post an asynchronous close request to a native window.
  void platform_post_close(void* native_handle);

  // Invalidate the entire client area of a native window so it repaints on the
  // next message-loop iteration. Used after focus changes to update the outline.
  void platform_invalidate(void* native_handle);

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
  uint32_t platform_dnd_begin_drag(void* native_handle,
                                    neui_detail::DataItem* item,
                                    uint32_t allowed_actions);

  // -------------------------------------------------------------------------
  // Native menu bar support.
  // All hmenu / parent_hmenu / submenu parameters are HMENU on Win32 and
  // opaque void* (always nullptr) on non-Windows platforms.

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
  // Switching the cursor mid-widget (e.g. when hovering a column-resize
  // divider in the GRID header band). One call per change; the platform
  // is expected to track the active cursor and reapply on every
  // WM_SETCURSOR / cursor-update message so the cursor stays sticky
  // until set_cursor is called again.
  //
  // Default kind reverts to the OS arrow.
  enum CursorKind {
    NEUI_CURSOR_DEFAULT   = 0,
    NEUI_CURSOR_EW_RESIZE = 1,   // double-headed horizontal arrow (column resize)
  };

  void platform_set_cursor(int kind /* CursorKind */);

} // namespace xpl_host
