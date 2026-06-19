#pragma once

#include <variant>
#include <string>
#include <vector>
#include <utility>

namespace neui
{
  class mujson
  {
  public:
    // A value is one of: null, bool, int, double, string, nested object, or
    // array. `null` is represented by std::monostate (also the default-
    // constructed state). Recursion is carried through `node` (the vector-
    // wrapper pattern lets the variant size itself without `node` being complete).
    struct node;
    using object_t = std::vector<std::pair<std::string, node>>;
    using array_t  = std::vector<node>;
    using value_t  = std::variant<std::monostate, bool, int, double, std::string, object_t, array_t>;
    struct node { value_t value; };

    // An entry of an object: { key, value-node }.
    using item_t = std::pair<std::string, node>;

    // Parse a json-like string. Returns the top-level object, or an empty
    // object on error (inspect getLastError()).
    [[nodiscard]] static object_t parse(const std::string& s) { return parse(s.c_str()); }
    [[nodiscard]] static object_t parse(const char* s);
    static const char* getLastError();

    // Serialize an object back to strict JSON (always quoted keys, escaped
    // strings, no trailing commas). `indent` is spaces-per-level for
    // pretty-printing; 0 (default) emits compact single-line output.
    [[nodiscard]] static std::string serialize(const object_t& obj, int indent = 0);
  };
}
