// Null platform layer for the crossplatform host.
// Used on non-Windows targets where no native windowing is available.
// All functions are no-ops; platform_run() returns immediately.

#include "host.h"
#include "platform.h"
#include <neui/neui.h>

// Full DataItem definition for the item-based clipboard stubs below.
#include "../shared/clipboard_item.h"

// Include the null backend.
#include "../../backends/null/null_backend.h"

namespace xpl_host
{
  void platform_init() {}

  neui_render_backend_t* platform_get_backend()
  {
    return neui_null_backend::get_backend();
  }

  void platform_create_appwindow(Session* /*session*/, uint32_t /*widget_index*/,
                                  WidgetData& /*wd*/) {}

  void platform_create_plugwindow(Session* /*session*/, uint32_t /*widget_index*/,
                                   WidgetData& /*wd*/) {}

  void platform_create_dialog(Session* /*session*/, uint32_t /*widget_index*/,
                               WidgetData& /*wd*/, void* /*owner_native*/) {}

  void platform_destroy_window(WidgetData& /*wd*/) {}

  void platform_show_window(void* /*native_handle*/) {}
  void platform_hide_window(void* /*native_handle*/) {}

  void platform_set_window_enabled(void* /*native_handle*/, bool /*enabled*/) {}
  void platform_activate_window(void* /*native_handle*/) {}

  void platform_set_window_title(void* /*native_handle*/, const char* /*text*/) {}

  void platform_set_window_pos(void* /*native_handle*/,
                                int /*x*/, int /*y*/, int /*w*/, int /*h*/,
                                uint32_t /*dpi*/) {}

  void platform_post_close(void* /*native_handle*/) {}

  float platform_get_scale_factor(void* /*native_handle*/) { return 1.0f; }

  void platform_invalidate(void* /*native_handle*/) {}

  bool platform_run() { return true; }
  bool platform_pump_once() { return true; }
  bool platform_run_modal_until(bool* /*keep_running*/) { return true; }

  // Menu bar - all no-ops on non-Windows platforms.
  bool  platform_menubar_in_frame()                                                     { return false; }
  void* platform_menubar_create(uint32_t /*widget_id*/)                                 { return nullptr; }
  void  platform_menubar_destroy(void* /*hmenu*/)                                       {}
  void  platform_menubar_attach(void* /*frame*/, void* /*hmenu*/)                       {}
  void  platform_menubar_refresh(void* /*frame*/)                                       {}
  void* platform_menubar_add_popup(void* /*hmenu*/, const char* /*text*/)               { return nullptr; }
  void  platform_menubar_add_item(void* /*hmenu*/, uint32_t /*cmd*/, const char* /*t*/) {}
  void  platform_menubar_add_separator(void* /*hmenu*/, uint32_t /*cmd*/)               {}
  void  platform_menubar_remove_popup(void* /*hmenu*/, void* /*sub*/)                   {}
  void  platform_menubar_remove_item(void* /*hmenu*/, uint32_t /*cmd*/)                 {}
  void  platform_menubar_enable_item(void* /*hmenu*/, uint32_t /*cmd*/, bool /*en*/)    {}
  void  platform_menubar_enable_popup(void* /*hmenu*/, void* /*sub*/, bool /*en*/)      {}
  void  platform_menubar_set_item_text(void* /*hmenu*/, uint32_t /*cmd*/, const char* /*t*/) {}
  void  platform_menubar_set_item_shortcut(void* /*hmenu*/, uint32_t /*cmd*/,
                                            uint32_t /*mods*/, uint32_t /*key*/)        {}

  void platform_set_window_icon(WidgetData& /*wd*/, const char* /*path*/) {}
  void platform_apply_size_constraints(void* /*nh*/, int /*minw*/, int /*minh*/,
                                        int /*maxw*/, int /*maxh*/) {}

  uint8_t* platform_load_image(const char* /*path*/,
                                uint32_t* /*width_out*/, uint32_t* /*height_out*/)
  {
    return nullptr;
  }

  void platform_free_image(uint8_t* /*pixels*/) {}

  // System clipboard - no-ops on platforms without one.
  bool platform_clipboard_set_text(const char* /*utf8*/, uint32_t /*length*/) { return false; }
  int  platform_clipboard_get_text(char* /*buf*/, int /*buflen*/)             { return 0;     }
  bool platform_clipboard_has_text()                                          { return false; }
  void platform_clipboard_set_primary(const char* /*u*/, uint32_t /*l*/)      {}
  int  platform_clipboard_get_primary(char* /*b*/, int /*n*/)                 { return 0; }
  bool platform_clipboard_write_item(const neui_detail::DataItem& /*item*/)   { return false; }
  bool platform_clipboard_read_item(neui_detail::DataItem& /*item*/)          { return false; }

  // Drag & drop - no-op on platforms without window-system DnD support.
  bool platform_dnd_register_window(void* /*native_handle*/, void* /*session_ptr*/,
                                     uint32_t /*frame_widget_id*/)            { return false; }
  void platform_dnd_unregister_window(void* /*native_handle*/)                {}
  uint32_t platform_dnd_begin_drag(void* /*native_handle*/,
                                     neui_detail::DataItem* /*item*/,
                                     uint32_t /*allowed_actions*/,
                                     void* /*preview_native*/,
                                     int /*hot_x*/, int /*hot_y*/)            { return 0;     }
  void*    platform_make_drag_preview(const uint8_t* /*bgra_premul*/,
                                       uint32_t /*w_px*/, uint32_t /*h_px*/,
                                       float /*scale*/)                       { return nullptr; }

  void     platform_set_cursor(int /*kind*/)                                  {}

  // Toast animation - no platform timer on the null host; toasts will
  // still paint a single frame if the client invalidates manually.
  void platform_start_toast_animation(void* /*native_handle*/)                {}
  void platform_stop_toast_animation(void* /*native_handle*/)                 {}
  uint64_t platform_now_ms()                                                  { return 0; }

  // No surface to show a message box on; 0 = failure per the contract.
  int platform_message_box(void* /*native_handle*/, const char* /*text*/,
                           const char* /*caption*/, uint32_t /*flags*/)       { return 0; }

} // namespace xpl_host

