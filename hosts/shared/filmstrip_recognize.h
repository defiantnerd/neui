#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <variant>

// mujson lives in the core lib (src/). Same relative reference component_loader.h
// uses - hosts don't carry src/ on their include path.
#include "../../src/mujson.h"

// Filmstrip recognition helpers. There is NO reliable in-band marker that a
// PNG/JPG is a frame strip, so recognition is a layered, opt-in convention
// (see plans/filmstrip-assets.md, Part C):
//   1. explicit  - set_frame_layout / create_filmstrip_from_file(count>0).
//   2. sidecar   - a "<image>.json" (or "<base>.json") next to the file:
//                  { "frames": 100, "orientation": "vertical", "gutter": 0 }
//                  or { "cols": C, "rows": R, "gutter": G }.
//   3. filename  - a trailing token: knob_100frames / fader-128 / knob_f64 /
//                  strip128 / knob_100.
// Both layers are consulted by filmstrip_discover_from_path (used when
// create_filmstrip_from_file is asked to discover, frame_count == 0).
// (An aspect-ratio guess was considered and rejected - it's not reliable
// enough to be worth the false positives on tall non-strip images.)
//
// The pure parsers (filename / sidecar string / object) take no file IO so
// they unit-test directly; only filmstrip_discover_from_path reads the
// sidecar file.

namespace neui_detail
{
  struct FilmstripLayout
  {
    uint32_t cols   = 1;
    uint32_t rows   = 0;
    uint32_t gutter = 0;
    bool valid() const { return cols >= 1 && rows >= 1; }
  };

  // --- mujson value pluck (case-sensitive key) ---------------------------
  inline bool fr_obj_int(const neui::mujson::object_t& o, const char* key, long& out)
  {
    for (const auto& kv : o)
      if (kv.first == key) {
        if (auto* p = std::get_if<int>(&kv.second.value))    { out = *p; return true; }
        if (auto* p = std::get_if<double>(&kv.second.value)) { out = static_cast<long>(*p); return true; }
      }
    return false;
  }
  inline const std::string* fr_obj_str(const neui::mujson::object_t& o, const char* key)
  {
    for (const auto& kv : o)
      if (kv.first == key)
        if (auto* p = std::get_if<std::string>(&kv.second.value)) return p;
    return nullptr;
  }

  // Build a layout from a parsed object. Accepts { cols, rows, gutter? } OR
  // { frames, orientation?, gutter? } (orientation "horizontal"/"h" => row
  // strip, else column strip). Returns false if neither shape is present.
  inline bool filmstrip_layout_from_object(const neui::mujson::object_t& o,
                                           FilmstripLayout& out)
  {
    long cols = 0, rows = 0, gutter = 0, frames = 0;
    const bool has_cols = fr_obj_int(o, "cols", cols);
    const bool has_rows = fr_obj_int(o, "rows", rows);
    fr_obj_int(o, "gutter", gutter);
    if (gutter < 0) gutter = 0;

    if (has_cols && has_rows && cols >= 1 && rows >= 1) {
      out.cols = static_cast<uint32_t>(cols);
      out.rows = static_cast<uint32_t>(rows);
      out.gutter = static_cast<uint32_t>(gutter);
      return true;
    }
    if (fr_obj_int(o, "frames", frames) && frames >= 1) {
      const std::string* ori = fr_obj_str(o, "orientation");
      const bool horiz = ori && (*ori == "horizontal" || *ori == "h");
      out.cols = horiz ? static_cast<uint32_t>(frames) : 1u;
      out.rows = horiz ? 1u : static_cast<uint32_t>(frames);
      out.gutter = static_cast<uint32_t>(gutter);
      return true;
    }
    return false;
  }

  inline bool filmstrip_parse_sidecar(const std::string& json, FilmstripLayout& out)
  {
    neui::mujson::object_t o = neui::mujson::parse(json);
    if (o.empty()) return false;
    return filmstrip_layout_from_object(o, out);
  }

  // Parse a trailing frame-count token from a base filename (no directory, no
  // extension). Recognizes <N>frames / <N>frame (keyword after the number)
  // and a number at the very end preceded by a separator or marker:
  //   _<N>, -<N>, _f<N>, -f<N>, strip<N>, _strip<N>, -strip<N>.
  // Requires N >= 2 (a 1-frame strip is pointless and would match "_v1").
  // Conservative: an unmarked trailing number (e.g. "image2") is rejected.
  inline bool filmstrip_parse_filename(const std::string& base_in, uint32_t& count_out)
  {
    std::string s;
    s.reserve(base_in.size());
    for (char c : base_in) s.push_back(static_cast<char>(std::tolower((unsigned char)c)));

    auto ends_with = [](const std::string& a, const char* b) {
      size_t n = std::strlen(b);
      return a.size() >= n && a.compare(a.size() - n, n, b) == 0;
    };

    bool kw = false;
    if (ends_with(s, "frames")) { s.erase(s.size() - 6); kw = true; }
    else if (ends_with(s, "frame")) { s.erase(s.size() - 5); kw = true; }

    size_t i = s.size();
    while (i > 0 && s[i - 1] >= '0' && s[i - 1] <= '9') --i;
    if (i == s.size()) return false;                 // no trailing digits
    const std::string digits = s.substr(i);
    const std::string rem    = s.substr(0, i);
    const long n = std::strtol(digits.c_str(), nullptr, 10);
    if (n < 2) return false;

    bool ok = kw;
    if (!ok) {
      if (ends_with(rem, "_") || ends_with(rem, "-")) ok = true;
      else if (ends_with(rem, "_f") || ends_with(rem, "-f")
            || ends_with(rem, "strip")) ok = true;
    }
    if (!ok) return false;
    count_out = static_cast<uint32_t>(n);
    return true;
  }

  // Discovery for create_filmstrip_from_file(frame_count == 0): try a
  // "<path>.json" then "<base>.json" sidecar, then the filename token (which
  // yields a count only, so default_horizontal picks the axis). Returns false
  // if nothing matches.
  inline bool filmstrip_discover_from_path(const std::string& path,
                                           bool default_horizontal,
                                           FilmstripLayout& out)
  {
    auto try_sidecar = [&](const std::string& p) -> bool {
      std::ifstream f(p, std::ios::binary);
      if (!f) return false;
      std::ostringstream ss;
      ss << f.rdbuf();
      return filmstrip_parse_sidecar(ss.str(), out);
    };
    if (try_sidecar(path + ".json")) return true;

    // "<base>.json" (image extension replaced).
    std::string base = path;
    const size_t dot = base.find_last_of('.');
    const size_t sl  = base.find_last_of("/\\");
    if (dot != std::string::npos && (sl == std::string::npos || dot > sl)) {
      base = base.substr(0, dot);
      if (try_sidecar(base + ".json")) return true;
    }

    // Filename token (strip directory + extension first).
    std::string fname = path;
    const size_t s2 = fname.find_last_of("/\\");
    if (s2 != std::string::npos) fname = fname.substr(s2 + 1);
    const size_t d2 = fname.find_last_of('.');
    if (d2 != std::string::npos) fname = fname.substr(0, d2);
    uint32_t count = 0;
    if (filmstrip_parse_filename(fname, count)) {
      out.cols = default_horizontal ? count : 1u;
      out.rows = default_horizontal ? 1u : count;
      out.gutter = 0;
      return true;
    }
    return false;
  }
} // namespace neui_detail
