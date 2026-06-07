#pragma once

#include <vector>
#include <string>
#include <cstdint>

#include <neui/neui.h>
#include "clipboard_item.h"

// Host-agnostic drag&drop dispatch. The per-host Session::dispatch_dnd_*
// methods are thin wrappers over the dnd_dispatch_* templates below - the
// ENTER / re-target / MOVE / LEAVE / DROP state machine lives here once.
//
// SessionT must expose:
//   // Pick the deepest visible+enabled drop_target under (x, y) within
//   // the frame, falling back to the frame itself. Returns the widget
//   // index (0 = none) and, via out params, its frame-local top-left.
//   uint32_t dnd_find_target(uint32_t frame_widget_idx, int x, int y,
//                            const char* const* formats, uint32_t count,
//                            int& out_abs_x, int& out_abs_y);
//   // Build + fire the NEUI_EVENT_DND_* event with widget-local coords
//   // (frame_x - abs_x, frame_y - abs_y), bracketing the client callback
//   // with _in_dnd_dispatch = true/false.
//   void dnd_send_event(uint32_t widget_idx, uint32_t event_type,
//                       int frame_x, int frame_y, int abs_x, int abs_y,
//                       const char* const* formats, uint32_t count,
//                       uint32_t suggested, uint32_t buttonmap,
//                       neui_data_item_t data_item);
//   // Fields: _widgets (.exists(idx)), _current_drop_target,
//   //         _current_drop_abs_x / _current_drop_abs_y,
//   //         _last_accepted_action, _data_items (DataItemStore).
//
// The hit-test walkers stay per-host on purpose: the xpl host hit-tests
// through the virtual WidgetData::hit_test using cached abs coords, while
// the native hosts accumulate parent-relative x/y on the fly - unifying
// them would change the hit-test contract for no gain.

namespace neui_detail
{
  // True if `accepted` is empty (wildcard) or contains any MIME from `formats`.
  inline bool dnd_formats_match(const std::vector<std::string>& accepted,
                                 const char* const* formats,
                                 uint32_t formats_count)
  {
    if (accepted.empty()) return true;
    if (!formats || formats_count == 0) return false;
    for (auto& want : accepted) {
      for (uint32_t i = 0; i < formats_count; ++i) {
        if (formats[i] && want == formats[i]) return true;
      }
    }
    return false;
  }

  template <typename SessionT>
  inline uint32_t dnd_dispatch_enter(SessionT* s, uint32_t frame_widget_idx,
                                      int x, int y,
                                      const char* const* formats, uint32_t count,
                                      uint32_t suggested, uint32_t buttonmap)
  {
    int abs_x = 0, abs_y = 0;
    uint32_t idx = s->dnd_find_target(frame_widget_idx, x, y, formats, count,
                                       abs_x, abs_y);
    s->_current_drop_target  = idx;
    s->_current_drop_abs_x   = abs_x;
    s->_current_drop_abs_y   = abs_y;
    s->_last_accepted_action = 0;
    if (idx == 0) return 0;
    s->dnd_send_event(idx, NEUI_EVENT_DND_ENTER, x, y, abs_x, abs_y,
                      formats, count, suggested, buttonmap,
                      neui_data_item_none);
    return s->_last_accepted_action;
  }

  template <typename SessionT>
  inline uint32_t dnd_dispatch_move(SessionT* s, uint32_t frame_widget_idx,
                                     int x, int y,
                                     const char* const* formats, uint32_t count,
                                     uint32_t suggested, uint32_t buttonmap)
  {
    int abs_x = 0, abs_y = 0;
    uint32_t idx = s->dnd_find_target(frame_widget_idx, x, y, formats, count,
                                       abs_x, abs_y);
    if (idx != s->_current_drop_target) {
      // Target changed - fire LEAVE on the old (if any) and ENTER on the new.
      if (s->_current_drop_target != 0 && s->_current_drop_target != UINT32_MAX &&
          s->_widgets.exists(s->_current_drop_target)) {
        s->dnd_send_event(s->_current_drop_target, NEUI_EVENT_DND_LEAVE,
                          x, y, s->_current_drop_abs_x, s->_current_drop_abs_y,
                          nullptr, 0, 0, 0, neui_data_item_none);
      }
      s->_current_drop_target  = idx;
      s->_current_drop_abs_x   = abs_x;
      s->_current_drop_abs_y   = abs_y;
      s->_last_accepted_action = 0;
      if (idx == 0) return 0;
      s->dnd_send_event(idx, NEUI_EVENT_DND_ENTER, x, y, abs_x, abs_y,
                        formats, count, suggested, buttonmap,
                        neui_data_item_none);
      return s->_last_accepted_action;
    }
    if (idx == 0) return 0;
    s->dnd_send_event(idx, NEUI_EVENT_DND_MOVE, x, y, abs_x, abs_y,
                      formats, count, suggested, buttonmap,
                      neui_data_item_none);
    return s->_last_accepted_action;
  }

  template <typename SessionT>
  inline void dnd_dispatch_leave(SessionT* s)
  {
    if (s->_current_drop_target != 0 && s->_current_drop_target != UINT32_MAX &&
        s->_widgets.exists(s->_current_drop_target)) {
      s->dnd_send_event(s->_current_drop_target, NEUI_EVENT_DND_LEAVE,
                        0, 0, s->_current_drop_abs_x, s->_current_drop_abs_y,
                        nullptr, 0, 0, 0, neui_data_item_none);
    }
    s->_current_drop_target  = UINT32_MAX;
    s->_current_drop_abs_x   = 0;
    s->_current_drop_abs_y   = 0;
    s->_last_accepted_action = 0;
  }

  template <typename SessionT>
  inline uint32_t dnd_dispatch_drop(SessionT* s,
                                     int x, int y,
                                     const char* const* formats, uint32_t count,
                                     uint32_t suggested, uint32_t buttonmap,
                                     DataItem* drop_item)
  {
    if (s->_current_drop_target == 0 || s->_current_drop_target == UINT32_MAX ||
        !s->_widgets.exists(s->_current_drop_target)) {
      s->_current_drop_target  = UINT32_MAX;
      s->_current_drop_abs_x   = 0;
      s->_current_drop_abs_y   = 0;
      s->_last_accepted_action = 0;
      return 0;
    }

    // Materialise the drop_item into the session's store so the client
    // can read it via clipboard_api->item_get_format during dispatch.
    // Released right after the callback returns.
    uint32_t item_id = 0;
    if (drop_item) {
      item_id = s->_data_items.allocate();
      auto* slot = s->_data_items.get(item_id);
      if (slot) {
        drop_item->for_each_format([&](const std::string& mime,
                                        const std::vector<uint8_t>& bytes) {
          slot->set_format(mime, bytes.data(),
                           static_cast<uint32_t>(bytes.size()));
        });
      } else {
        item_id = 0;
      }
    }

    s->dnd_send_event(s->_current_drop_target, NEUI_EVENT_DND_DROP,
                      x, y, s->_current_drop_abs_x, s->_current_drop_abs_y,
                      formats, count, suggested, buttonmap,
                      neui_data_item_t{ item_id });

    if (item_id) s->_data_items.release(item_id);

    uint32_t action = s->_last_accepted_action;
    // A drop ends the drag session; clear state.
    s->_current_drop_target  = UINT32_MAX;
    s->_current_drop_abs_x   = 0;
    s->_current_drop_abs_y   = 0;
    s->_last_accepted_action = 0;
    return action;
  }

} // namespace neui_detail
