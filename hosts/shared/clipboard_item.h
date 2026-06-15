#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Shared MIME-typed data-item storage.
//
// A DataItem holds a map of mime-type -> bytes; a DataItemStore is a
// per-session, slot-reused vector that hands out 1-based item ids
// (0 / UINT32_MAX = invalid). The same primitive backs:
//   - the system-clipboard item-based API (neui_data_item_t in clipboard.h),
//   - drag&drop drop payloads (transient items materialised during DROP
//     dispatch and released right after).
//
// Header-only (matches hosts/shared/attrs.h) so both host static libs can
// include it without ODR concerns.

namespace neui_detail
{
  // Provider callback signature - mirrors the public C ABI in
  // include/neui/d/clipboard.h::neui_data_provider_t verbatim. Kept as a
  // raw function pointer (not std::function) so the storage stays POD and
  // matches the C-side type exactly.
  using DataProviderFn = const uint8_t* (*)(void* userdata,
                                             const char* mime,
                                             uint32_t* out_size);

  class DataItem
  {
  public:
    void set_format(const std::string& mime, const void* data, uint32_t length) {
      auto& e = _formats[mime];
      e.bytes.assign(static_cast<const uint8_t*>(data),
                     static_cast<const uint8_t*>(data) + length);
      e.provider     = nullptr;
      e.userdata     = nullptr;
      e.materialised = true;
    }

    // Lazy variant - the bytes are produced by `provider(userdata, mime, &size)`
    // on first read. Calling set_format on the same mime later switches the
    // entry back to eager bytes.
    void set_format_provider(const std::string& mime,
                              DataProviderFn provider, void* userdata) {
      auto& e = _formats[mime];
      e.bytes.clear();
      e.provider     = provider;
      e.userdata     = userdata;
      e.materialised = false;
    }

    // Returns total byte count when buf is null. Copies up to buflen bytes
    // when buf is non-null. Returns 0 if format absent. Materialises lazy
    // entries on first call (cached for the rest of the item's lifetime).
    int get_format(const std::string& mime, void* buf, int buflen) const {
      auto it = _formats.find(mime);
      if (it == _formats.end()) return 0;
      ensure_materialised(it->second, mime);
      int n = static_cast<int>(it->second.bytes.size());
      if (buf && buflen > 0) {
        int copy = (buflen < n) ? buflen : n;
        std::memcpy(buf, it->second.bytes.data(), static_cast<size_t>(copy));
      }
      return n;
    }

    bool has_format(const std::string& mime) const {
      return _formats.find(mime) != _formats.end();
    }

    // True if the named format is registered as a lazy provider that has
    // not been materialised yet. Used by the drag-source layers to register
    // the format with the OS as a deferred-render entry instead of pre-
    // encoding bytes at snapshot time.
    bool is_lazy_format(const std::string& mime) const {
      auto it = _formats.find(mime);
      if (it == _formats.end()) return false;
      return !it->second.materialised && it->second.provider != nullptr;
    }

    // Read out the provider + userdata pair without materialising. Used by
    // the macOS drag-source / clipboard adapters to copy the (callback,
    // userdata) tuple into an Obj-C delegate that AppKit retains for the
    // lifetime of the pasteboard / drag session. Returns false if the
    // format is absent or is eager bytes (not a provider).
    bool get_lazy_provider(const std::string& mime,
                            DataProviderFn* out_fn, void** out_userdata) const {
      auto it = _formats.find(mime);
      if (it == _formats.end()) return false;
      if (it->second.materialised || !it->second.provider) return false;
      if (out_fn) *out_fn = it->second.provider;
      if (out_userdata) *out_userdata = it->second.userdata;
      return true;
    }

    // Iterate over (mime, bytes) pairs. Materialises lazy entries on the
    // fly so the consumer always sees concrete bytes (the bytes cache is
    // `mutable`, so this stays a logical-const operation). Used by
    // clipboard write() and DnD dispatch to enumerate everything an item
    // carries.
    template <typename F>
    void for_each_format(F&& fn) const {
      for (auto& kv : _formats) {
        ensure_materialised(kv.second, kv.first);
        fn(kv.first, kv.second.bytes);
      }
    }

    // MIME-only enumeration - does NOT materialise lazy entries. Used by
    // the drag-source path to register format names with the OS without
    // forcing eager byte production.
    template <typename F>
    void for_each_mime(F&& fn) const {
      for (auto& kv : _formats) fn(kv.first);
    }

  private:
    // The byte cache + the materialised flag are `mutable` so const-context
    // reads (get_format / for_each_format on a const DataItem&) can lazily
    // produce bytes - the textbook caching-mutable pattern. provider /
    // userdata are not mutable: switching from lazy to eager (or vice
    // versa) requires the non-const set_format / set_format_provider.
    struct Entry {
      mutable std::vector<uint8_t> bytes;
      DataProviderFn               provider     = nullptr;
      void*                        userdata     = nullptr;
      mutable bool                 materialised = false;
    };

    static void ensure_materialised(const Entry& e, const std::string& mime) {
      if (e.materialised || !e.provider) return;
      uint32_t size = 0;
      const uint8_t* p = e.provider(e.userdata, mime.c_str(), &size);
      if (p && size > 0) {
        e.bytes.assign(p, p + size);
      }
      // Cache even on provider returning null/0 - the producer has spoken;
      // a second call would just re-do the work. The cached entry stays
      // empty (get_format returns 0, matching "format absent" semantics).
      e.materialised = true;
    }

    std::unordered_map<std::string, Entry> _formats;
  };

  // Per-session item table: 1-based ids, slot-reused on release.
  class DataItemStore
  {
  public:
    // Returns a new item id (>= 1).
    uint32_t allocate() {
      for (size_t i = 0; i < _slots.size(); ++i) {
        if (!_slots[i]) {
          _slots[i] = std::make_unique<DataItem>();
          return static_cast<uint32_t>(i + 1);
        }
      }
      _slots.push_back(std::make_unique<DataItem>());
      return static_cast<uint32_t>(_slots.size());
    }

    void release(uint32_t id) {
      if (id == 0 || id == UINT32_MAX) return;
      size_t idx = id - 1;
      if (idx < _slots.size()) _slots[idx].reset();
    }

    // Returns nullptr if id invalid or already released.
    DataItem* get(uint32_t id) {
      if (id == 0 || id == UINT32_MAX) return nullptr;
      size_t idx = id - 1;
      if (idx >= _slots.size()) return nullptr;
      return _slots[idx].get();
    }

  private:
    std::vector<std::unique_ptr<DataItem>> _slots;
  };

} // namespace neui_detail
