#pragma once

#include <vector>
#include <string>
#include <cstdint>

#include "clipboard_item.h"

// Host-agnostic drag&drop dispatch helpers. The per-host Session::dispatch_dnd_*
// methods are thin wrappers over these. The Session/WidgetData types are
// passed via templates so the same code works for the xpl, win32, and macos
// hosts (each has its own WidgetData layout but they all carry the same
// drop_target / accepted_mimes / abs_x / abs_y / widget_id / hit_test fields).

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

} // namespace neui_detail
