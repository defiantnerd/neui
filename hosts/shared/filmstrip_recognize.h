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
#include "mujson_accessors.h"  // obj_get / as_num / as_str (shared with component_loader)

// Filmstrip recognition helpers. There is NO reliable in-band marker that a
// PNG/JPG is a frame strip, so recognition is a layered, opt-in convention
// (see plans/filmstrip-assets.md, Part C):
//   1. explicit  - set_frame_layout / create_filmstrip_from_file(count>0).
//   2. sidecar   - a "<image>.json" (or "<base>.json") next to the file:
//                  { "frames": 100, "orientation": "vertical", "gutter": 0 }
//                  or { "cols": C, "rows": R, "gutter": G }. A bare "frames"
//                  with no "orientation" key defaults to a VERTICAL strip (the
//                  audio-plugin convention) - the sidecar is authoritative, so
//                  a horizontal strip must say so explicitly.
//   3. filename  - a trailing <N>frames / <N>frame / <N>f token (the discover
//                  axis comes from create_filmstrip_from_file's orientation arg,
//                  which defaults to vertical).
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

  // Read a key as a positive grid count. Rejects absent / non-positive values
  // AND anything that doesn't fit a uint32 - without the upper bound an absurd
  // sidecar (e.g. {"frames": 4294967297}) would WRAP on the cast to a small
  // bogus count (1) that set_frame_layout would happily accept.
  inline bool fr_count(const neui::mujson::object_t& o, const char* key, uint32_t& out)
  {
    double d;
    if (!as_num(obj_get(o, key), d)) return false;
    if (!(d >= 1.0 && d <= 4294967295.0)) return false;
    out = static_cast<uint32_t>(d);
    return true;
  }
  inline uint32_t fr_gutter(const neui::mujson::object_t& o)
  {
    double d;
    if (!as_num(obj_get(o, "gutter"), d)) return 0u;
    if (d <= 0.0 || d > 4294967295.0) return 0u;   // negative / absurd -> none
    return static_cast<uint32_t>(d);
  }

  // Build a layout from a parsed object. Accepts { cols, rows, gutter? } OR
  // { frames, orientation?, gutter? } (orientation "horizontal"/"h" => row
  // strip, else column strip). Returns false if neither shape is present.
  inline bool filmstrip_layout_from_object(const neui::mujson::object_t& o,
                                           FilmstripLayout& out)
  {
    const uint32_t gutter = fr_gutter(o);

    uint32_t cols = 0, rows = 0;
    if (fr_count(o, "cols", cols) && fr_count(o, "rows", rows)) {
      out.cols = cols; out.rows = rows; out.gutter = gutter;
      return true;
    }
    uint32_t frames = 0;
    if (fr_count(o, "frames", frames)) {
      const std::string* ori = as_str(obj_get(o, "orientation"));
      const bool horiz = ori && (*ori == "horizontal" || *ori == "h");
      out.cols = horiz ? frames : 1u;
      out.rows = horiz ? 1u : frames;
      out.gutter = gutter;
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
  // extension). Requires the number to be immediately followed by an explicit
  // frame keyword at the very end of the name: <N>frames / <N>frame / <N>f
  //   knob_100frames, KNOB_64FRAME, fader-128f.
  // Requires N >= 2 (a 1-frame strip is pointless). Deliberately conservative:
  // a bare trailing number with no keyword (knob_100, fader-128, image2) and an
  // incidental word match (airstrip5, logo-2024) are rejected - those shapes
  // are far too common as ordinary asset names to auto-slice into a strip.
  inline bool filmstrip_parse_filename(const std::string& base_in, uint32_t& count_out)
  {
    std::string s;
    s.reserve(base_in.size());
    for (char c : base_in) s.push_back(static_cast<char>(std::tolower((unsigned char)c)));

    auto ends_with = [](const std::string& a, const char* b) {
      size_t n = std::strlen(b);
      return a.size() >= n && a.compare(a.size() - n, n, b) == 0;
    };

    // Strip the required trailing keyword (longest first). No keyword => not a
    // strip filename.
    if      (ends_with(s, "frames")) s.erase(s.size() - 6);
    else if (ends_with(s, "frame"))  s.erase(s.size() - 5);
    else if (ends_with(s, "f"))      s.erase(s.size() - 1);
    else return false;

    size_t i = s.size();
    while (i > 0 && s[i - 1] >= '0' && s[i - 1] <= '9') --i;
    if (i == s.size()) return false;                 // keyword not preceded by a number
    const long n = std::strtol(s.c_str() + i, nullptr, 10);
    if (n < 2) return false;
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
