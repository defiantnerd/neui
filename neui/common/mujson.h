#pragma once

#include <variant>
#include <string>
#include <vector>
#include <tuple>

/*
*   mujson
* 
*   - no arrays
*   - no maps
*   - simple parser
*   - key is always string
*   - string/int only for the value

*/

namespace neui
{
  struct mujson
  {
    using value_t = std::variant<std::string, int>;
    using item_t = std::tuple<std::string, value_t>;
    [[nodiscard]] static std::vector<item_t> parse(const char* s);
    [[nodiscard]] static std::vector<item_t> parse(const std::string& s) { return parse(s.c_str()); }
  };
}
