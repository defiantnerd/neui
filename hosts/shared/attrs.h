#pragma once

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>

#include <neui/d/attrs.h>
#include <neui/d/grid.h>

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

  // ---- Well-known attribute kind registry ----------------------------------
  //
  // The attribute API is type-strict (set_int / set_float / set_string each
  // store with a distinct Kind and the strict getters return the default on
  // a mismatch). That works well for the host but is a footgun for clients:
  // calling `attrs->set_int(NEUI_ATTR_FONT_SIZE, 18)` against a documented-
  // FLOAT key silently no-ops at read time because get_float returns the
  // default. To catch that at the call site, the AttrBag setters consult
  // the table below and assert in debug builds when a well-known key is
  // paired with the wrong Kind. Release builds skip the check entirely
  // (NDEBUG) so the runtime cost is zero.
  //
  // Adding a new NEUI_ATTR_* macro to include/neui/d/attrs.h REQUIRES a
  // matching row here so the assert covers it. Unknown keys are not in the
  // table and remain inert per the documented contract (CLAUDE.md).

  enum class AttrKind : uint8_t { INT, FLOAT, STRING };

  struct WellKnownAttr { const char* key; AttrKind kind; };

  // Order matches the documentation table in CLAUDE.md / include/neui/d/attrs.h
  // so diffs stay readable when new attrs are added.
  inline constexpr WellKnownAttr k_well_known_attrs[] = {
    { NEUI_ATTR_TRISTATE,             AttrKind::INT    },
    { NEUI_ATTR_MULTILINE,            AttrKind::INT    },
    { NEUI_ATTR_READONLY,             AttrKind::INT    },
    { NEUI_ATTR_PASSWORD,             AttrKind::INT    },
    { NEUI_ATTR_TAB_STOP,             AttrKind::INT    },
    { NEUI_ATTR_ALIGN_TEXT,           AttrKind::STRING },
    { NEUI_ATTR_MIN_WIDTH,            AttrKind::INT    },
    { NEUI_ATTR_MIN_HEIGHT,           AttrKind::INT    },
    { NEUI_ATTR_MAX_WIDTH,            AttrKind::INT    },
    { NEUI_ATTR_MAX_HEIGHT,           AttrKind::INT    },
    { NEUI_ATTR_ICON_PATH,            AttrKind::STRING },
    { NEUI_ATTR_ROTATION,             AttrKind::FLOAT  },
    { NEUI_ATTR_BACKGROUND,           AttrKind::INT    },
    { NEUI_ATTR_THEME_MODE,           AttrKind::INT    },
    { NEUI_ATTR_FOLLOW_SYSTEM_THEME,  AttrKind::INT    },
    { NEUI_ATTR_ORIENTATION,          AttrKind::STRING },
    { NEUI_ATTR_POLARITY,             AttrKind::STRING },
    { NEUI_ATTR_STEPS,                AttrKind::INT    },
    { NEUI_ATTR_MODAL,                AttrKind::INT    },
    { NEUI_ATTR_VALUE_TEXT,           AttrKind::STRING },
    { NEUI_ATTR_KNOB_MODE,            AttrKind::INT    },
    { NEUI_ATTR_FONT_FAMILY,          AttrKind::STRING },
    { NEUI_ATTR_FONT_SIZE,            AttrKind::FLOAT  },
    { NEUI_ATTR_FONT_WEIGHT,          AttrKind::INT    },
    { NEUI_PARAM_VALUE,               AttrKind::FLOAT  },
    { NEUI_PARAM_DEFAULT,             AttrKind::FLOAT  },
    { NEUI_ATTR_GRID_ROW_HEIGHT,              AttrKind::INT },
    { NEUI_ATTR_GRID_HEADER_HEIGHT,           AttrKind::INT },
    { NEUI_ATTR_GRID_FOCUS_ROW_COLOR,         AttrKind::INT },
    { NEUI_ATTR_GRID_SHOW_FOCUS_ROW,          AttrKind::INT },
    { NEUI_ATTR_GRID_COLUMN_MIN_WIDTH_DEFAULT,AttrKind::INT },
    { NEUI_ATTR_GRID_CELL_FOCUS,              AttrKind::INT },
  };

  // Lookup the documented kind for a well-known key. Returns nullptr when
  // the key is not in the table (unknown / client-namespaced keys).
  inline const AttrKind* well_known_attr_kind(const char* key)
  {
    if (!key) return nullptr;
    for (const auto& a : k_well_known_attrs)
      if (std::strcmp(a.key, key) == 0) return &a.kind;
    return nullptr;
  }

  // Debug-only assert: fires when a well-known key is set with the wrong
  // Kind. No-op in release. Prints a useful message to stderr before the
  // assert trips so the breakpoint / abort dialog points at the bad call.
  inline void assert_attr_kind(const char* key, AttrKind actual)
  {
  #ifndef NDEBUG
    const AttrKind* expected = well_known_attr_kind(key);
    if (!expected || *expected == actual) return;
    auto name = [](AttrKind k) -> const char* {
      switch (k) {
        case AttrKind::INT:    return "INT (set_int)";
        case AttrKind::FLOAT:  return "FLOAT (set_float)";
        case AttrKind::STRING: return "STRING (set_string)";
      }
      return "?";
    };
    std::fprintf(stderr,
      "neui attribute kind mismatch: key='%s' expected=%s actual=%s\n",
      key, name(*expected), name(actual));
    assert(false && "neui well-known attribute kind mismatch (see stderr)");
  #else
    (void)key; (void)actual;
  #endif
  }

  class AttrBag
  {
  public:
    void set_int(const std::string& key, int32_t v) {
      assert_attr_kind(key.c_str(), AttrKind::INT);
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
      assert_attr_kind(key.c_str(), AttrKind::FLOAT);
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
      assert_attr_kind(key.c_str(), AttrKind::STRING);
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
