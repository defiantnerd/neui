#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

// Shared attribute bag used by both hosts. Stored lazily on a widget so the
// per-widget overhead is one pointer when unused.

namespace neui_detail
{
  struct AttrValue
  {
    enum Kind : uint8_t { INT32, FLOAT, STRING };
    Kind        kind = INT32;
    int32_t     i    = 0;
    float       f    = 0.0f;
    std::string s;
  };

  class AttrBag
  {
  public:
    void set_int(const std::string& key, int32_t v) {
      auto& slot = _map[key];
      slot.kind = AttrValue::INT32;
      slot.i    = v;
    }

    int32_t get_int(const std::string& key, int32_t default_value) const {
      auto it = _map.find(key);
      if (it == _map.end() || it->second.kind != AttrValue::INT32)
        return default_value;
      return it->second.i;
    }

    void set_float(const std::string& key, float v) {
      auto& slot = _map[key];
      slot.kind = AttrValue::FLOAT;
      slot.f    = v;
    }

    float get_float(const std::string& key, float default_value) const {
      auto it = _map.find(key);
      if (it == _map.end() || it->second.kind != AttrValue::FLOAT)
        return default_value;
      return it->second.f;
    }

    void set_string(const std::string& key, const char* v) {
      auto& slot = _map[key];
      slot.kind = AttrValue::STRING;
      slot.s    = v ? v : "";
    }

    // Returned pointer valid until next set_string / remove / clear / widget
    // destroy. Returns nullptr if key absent or wrong kind.
    const char* get_string(const std::string& key) const {
      auto it = _map.find(key);
      if (it == _map.end() || it->second.kind != AttrValue::STRING)
        return nullptr;
      return it->second.s.c_str();
    }

    bool has(const std::string& key) const {
      return _map.find(key) != _map.end();
    }

    bool remove(const std::string& key) {
      return _map.erase(key) > 0;
    }

  private:
    std::unordered_map<std::string, AttrValue> _map;
  };

  // Helper: lazy allocator so widgets without attributes pay only a pointer.
  inline AttrBag& ensure_attrs(std::unique_ptr<AttrBag>& slot) {
    if (!slot) slot = std::make_unique<AttrBag>();
    return *slot;
  }

  inline const AttrBag* attrs_readonly(const std::unique_ptr<AttrBag>& slot) {
    return slot.get();
  }

  // Read an attribute as a float, promoting INT32 (i -> (float)i) and
  // treating STRING / missing as the default. Used by compound bindings
  // and any other consumer that wants permissive numeric reads (the
  // typed get_int / get_float on AttrBag itself stays strict).
  inline float attr_as_float(const AttrBag* bag, const std::string& key,
                              float default_value)
  {
    if (!bag) return default_value;
    if (bag->has(key)) {
      float f = bag->get_float(key, 0.0f);
      if (f != 0.0f) return f;
      // Could be a real 0.0f stored as float, or an int. Re-probe int.
      int32_t i = bag->get_int(key, 0);
      if (i != 0) return static_cast<float>(i);
      // Either an exact zero (FLOAT or INT32) or a string. Treat both as 0.0f.
      return 0.0f;
    }
    return default_value;
  }

} // namespace neui_detail
