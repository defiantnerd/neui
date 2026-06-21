#pragma once

#include <string>
#include <variant>

#include "../../src/mujson.h"

// Canonical mujson value accessors, shared by the filmstrip recognizer
// (filmstrip_recognize.h) and the component loader (component_loader.h). One
// home so the two JSON consumers can't drift in their int/double/string
// coercion rules - both accept the same shapes the same way.
namespace neui_detail
{
  inline const neui::mujson::node* obj_get(const neui::mujson::object_t& o,
                                           const char* key)
  {
    for (const auto& kv : o)
      if (kv.first == key) return &kv.second;
    return nullptr;
  }
  inline const neui::mujson::object_t* as_obj(const neui::mujson::node* n)
  {
    return n ? std::get_if<neui::mujson::object_t>(&n->value) : nullptr;
  }
  inline const neui::mujson::array_t* as_arr(const neui::mujson::node* n)
  {
    return n ? std::get_if<neui::mujson::array_t>(&n->value) : nullptr;
  }
  inline const std::string* as_str(const neui::mujson::node* n)
  {
    return n ? std::get_if<std::string>(&n->value) : nullptr;
  }
  // Accept BOTH int and double arms (bare 0 -> int, 0.5 -> double).
  inline bool as_num(const neui::mujson::node* n, double& out)
  {
    if (!n) return false;
    if (auto* i = std::get_if<int>(&n->value))    { out = *i; return true; }
    if (auto* d = std::get_if<double>(&n->value)) { out = *d; return true; }
    return false;
  }
  inline bool as_bool(const neui::mujson::node* n, bool def)
  {
    if (n) if (auto* b = std::get_if<bool>(&n->value)) return *b;
    return def;
  }
} // namespace neui_detail
