#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Clipboard item storage shared between hosts. An item holds a map of
// mime-type -> bytes; an ItemStore is a per-session, slot-reused vector that
// hands out 1-based item ids (0 / UINT32_MAX = invalid).
//
// Header-only (matches hosts/shared/attrs.h) so both host static libs can
// include it without ODR concerns.

namespace neui_detail
{
  class ClipboardItem
  {
  public:
    void set_format(const std::string& mime, const void* data, uint32_t length) {
      auto& bytes = _formats[mime];
      bytes.assign(static_cast<const uint8_t*>(data),
                   static_cast<const uint8_t*>(data) + length);
    }

    // Returns total byte count when buf is null. Copies up to buflen bytes
    // when buf is non-null. Returns 0 if format absent.
    int get_format(const std::string& mime, void* buf, int buflen) const {
      auto it = _formats.find(mime);
      if (it == _formats.end()) return 0;
      int n = static_cast<int>(it->second.size());
      if (buf && buflen > 0) {
        int copy = (buflen < n) ? buflen : n;
        std::memcpy(buf, it->second.data(), copy);
      }
      return n;
    }

    bool has_format(const std::string& mime) const {
      return _formats.find(mime) != _formats.end();
    }

  private:
    std::unordered_map<std::string, std::vector<uint8_t>> _formats;
  };

  // Per-session item table: 1-based ids, slot-reused on release.
  class ClipboardItemStore
  {
  public:
    // Returns a new item id (>= 1).
    uint32_t allocate() {
      for (size_t i = 0; i < _slots.size(); ++i) {
        if (!_slots[i]) {
          _slots[i] = std::make_unique<ClipboardItem>();
          return static_cast<uint32_t>(i + 1);
        }
      }
      _slots.push_back(std::make_unique<ClipboardItem>());
      return static_cast<uint32_t>(_slots.size());
    }

    void release(uint32_t id) {
      if (id == 0 || id == UINT32_MAX) return;
      size_t idx = id - 1;
      if (idx < _slots.size()) _slots[idx].reset();
    }

    // Returns nullptr if id invalid or already released.
    ClipboardItem* get(uint32_t id) {
      if (id == 0 || id == UINT32_MAX) return nullptr;
      size_t idx = id - 1;
      if (idx >= _slots.size()) return nullptr;
      return _slots[idx].get();
    }

  private:
    std::vector<std::unique_ptr<ClipboardItem>> _slots;
  };

} // namespace neui_detail
