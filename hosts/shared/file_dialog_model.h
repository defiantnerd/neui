#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <algorithm>

#include <neui/d/notify.h>

// Portable logic behind NEUI_API_NOTIFY::open_file / save_file.
//
// Everything here is pure string / list work with no filesystem and no
// platform calls, which is the point: the parts of a file dialog that are
// easy to get subtly wrong (glob matching, extension completion, listing
// order) are Tier-1 testable, and the per-platform code is left with only
// the parts that genuinely need an OS - enumerating a directory and
// putting pixels or a native panel on screen.
//
// Used by all three real platforms, not just the neui-drawn fallback:
//   - filter_spec / parse_filters feed IFileDialog's COMDLG_FILTERSPEC,
//     NSOpenPanel's allowedContentTypes, and the portal's a(sa(us)).
//   - complete_extension implements the documented save_file rule on the
//     platforms whose native panel does not do it for us.
//   - list_directory_view + sort is the neui-drawn browser's model.

namespace neui_detail
{
  // ---- Filters -------------------------------------------------------------

  struct FileFilter
  {
    std::string              label;
    std::vector<std::string> patterns;   // e.g. { "*.png", "*.PNG" }

    // True when this entry is the conventional "every file" escape hatch.
    // Native dialogs need to know: a "*" entry must NOT be turned into an
    // extension whitelist, or it stops matching everything.
    bool matches_everything() const
    {
      for (const auto& p : patterns)
        if (p == "*" || p == "*.*") return true;
      return false;
    }

    // The extension this filter completes a typed name with (no leading
    // dot), or "" when it has none to offer (a "*" filter, or a pattern
    // whose extension is itself wildcarded like "*.p?g").
    std::string default_extension() const
    {
      for (const auto& p : patterns) {
        if (p == "*" || p == "*.*") continue;
        size_t dot = p.rfind('.');
        if (dot == std::string::npos || dot + 1 >= p.size()) continue;
        std::string ext = p.substr(dot + 1);
        if (ext.find('*') != std::string::npos) continue;
        if (ext.find('?') != std::string::npos) continue;
        return ext;
      }
      return std::string();
    }
  };

  // Split a semicolon-separated pattern list. Empty runs and surrounding
  // spaces are dropped, so "*.png; *.jpg ;;" yields exactly two patterns.
  inline std::vector<std::string> parse_filter_patterns(const char* patterns)
  {
    std::vector<std::string> out;
    if (!patterns) return out;
    std::string cur;
    auto flush = [&]() {
      size_t b = cur.find_first_not_of(" \t");
      size_t e = cur.find_last_not_of(" \t");
      if (b != std::string::npos) out.push_back(cur.substr(b, e - b + 1));
      cur.clear();
    };
    for (const char* p = patterns;; ++p) {
      if (*p == ';' || *p == 0) { flush(); if (*p == 0) break; }
      else cur += *p;
    }
    return out;
  }

  // Decode the client's descriptor into the internal form. A NULL / empty
  // filter list yields an empty vector, which every consumer reads as
  // "no type filtering"; entries whose pattern list parses to nothing are
  // dropped rather than kept as a filter that matches no file at all.
  inline std::vector<FileFilter> parse_filters(const neui_file_dialog_t* desc)
  {
    std::vector<FileFilter> out;
    if (!desc || !desc->filters) return out;
    for (uint32_t i = 0; i < desc->filter_count; ++i) {
      FileFilter f;
      f.patterns = parse_filter_patterns(desc->filters[i].patterns);
      if (f.patterns.empty()) continue;
      f.label = desc->filters[i].label ? desc->filters[i].label : "";
      if (f.label.empty()) f.label = desc->filters[i].patterns;
      out.push_back(std::move(f));
    }
    return out;
  }

  // Clamp the client's default_filter to a usable index. Returns 0 for an
  // out-of-range value (documented behaviour) and for an empty list.
  inline size_t clamp_default_filter(const neui_file_dialog_t* desc,
                                     size_t filter_count)
  {
    if (!desc || filter_count == 0) return 0;
    return desc->default_filter < filter_count ? desc->default_filter : 0;
  }

  // ---- Glob matching -------------------------------------------------------

  inline char ascii_lower(char c)
  {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  }

  // `*` = any run including empty, `?` = exactly one character. ASCII
  // case-insensitive (see the header doc on neui_file_filter_t). Iterative
  // backtracking rather than recursion, so a pathological pattern like
  // "*a*a*a*a*b" cannot blow the stack on attacker-supplied input.
  inline bool glob_match(const char* name, const char* pattern)
  {
    if (!name || !pattern) return false;
    const char* n = name;
    const char* p = pattern;
    const char* star_p = nullptr;   // pattern position just after the last '*'
    const char* star_n = nullptr;   // name position that '*' was tried at
    while (*n) {
      if (*p == '?' || (*p && ascii_lower(*p) == ascii_lower(*n))) { ++p; ++n; }
      else if (*p == '*') { star_p = ++p; star_n = n; }
      else if (star_p)    { p = star_p; n = ++star_n; }
      else return false;
    }
    while (*p == '*') ++p;
    return *p == 0;
  }

  // True when `name` passes the filter (any one of its patterns matches).
  inline bool filter_accepts(const FileFilter& f, const std::string& name)
  {
    if (f.matches_everything()) return true;
    for (const auto& p : f.patterns)
      if (glob_match(name.c_str(), p.c_str())) return true;
    return false;
  }

  // ---- Paths ---------------------------------------------------------------
  //
  // POSIX-shaped ('/' separator) with Windows separators normalised on the
  // way in, because the only consumer that builds paths itself is the
  // neui-drawn browser (Linux) - the native panels hand back finished
  // absolute paths. `\` is therefore treated as a separator too, so a
  // client that passed a Windows-style initial_dir still lands somewhere
  // sensible.

  inline bool is_path_sep(char c) { return c == '/' || c == '\\'; }

  inline bool path_is_absolute(const std::string& p)
  {
    if (p.empty()) return false;
    if (is_path_sep(p[0])) return true;
    // "C:\..." / "C:/..."
    return p.size() >= 3 && p[1] == ':' && is_path_sep(p[2]);
  }

  // Join a directory and a leaf. A leaf that is itself absolute wins
  // outright (so join("/etc", "/tmp/x") == "/tmp/x"), matching what every
  // path API does and what a user typing an absolute name into the name
  // field expects.
  //
  // The inserted separator copies whichever style `dir` already uses, so
  // re-joining a Windows path taken apart by path_parent yields
  // "C:\dir\name" rather than the mixed "C:\dir/name". Both work when
  // handed back to the OS, but a client that string-matches on the result
  // should not have to cope with two styles in one path.
  inline std::string path_join(const std::string& dir, const std::string& leaf)
  {
    if (leaf.empty()) return dir;
    if (path_is_absolute(leaf)) return leaf;
    if (dir.empty()) return leaf;
    std::string out = dir;
    if (!is_path_sep(out.back())) {
      char sep = '/';
      for (size_t i = out.size(); i-- > 0;)
        if (is_path_sep(out[i])) { sep = out[i]; break; }
      out += sep;
    }
    out += leaf;
    return out;
  }

  // The containing directory, without a trailing separator ("/" stays
  // "/"). Returns the input unchanged when there is nowhere further up,
  // so a caller can loop on it without a special case.
  inline std::string path_parent(const std::string& p)
  {
    if (p.empty()) return p;
    size_t end = p.size();
    while (end > 1 && is_path_sep(p[end - 1])) --end;      // strip trailing
    size_t slash = std::string::npos;
    for (size_t i = end; i-- > 0;)
      if (is_path_sep(p[i])) { slash = i; break; }
    if (slash == std::string::npos) return p;              // relative leaf
    if (slash == 0) return "/";                            // "/foo" -> "/"
    return p.substr(0, slash);
  }

  // The final component ("/a/b/c" -> "c", "/a/b/" -> "b").
  inline std::string path_leaf(const std::string& p)
  {
    size_t end = p.size();
    while (end > 0 && is_path_sep(p[end - 1])) --end;
    size_t slash = std::string::npos;
    for (size_t i = end; i-- > 0;)
      if (is_path_sep(p[i])) { slash = i; break; }
    if (slash == std::string::npos) return p.substr(0, end);
    return p.substr(slash + 1, end - slash - 1);
  }

  // True when the leaf already carries an extension. A leading dot does
  // NOT count (".bashrc" is a hidden file with no extension, and
  // completing it to ".bashrc.preset" would be wrong).
  inline bool has_extension(const std::string& leaf)
  {
    size_t dot = leaf.rfind('.');
    return dot != std::string::npos && dot > 0 && dot + 1 < leaf.size();
  }

  // The documented save_file completion rule: append the active filter's
  // extension when the user typed none. A name that already has one is
  // returned unchanged even if it does not match the filter - second-
  // guessing a deliberate ".txt" into ".txt.preset" is worse than obeying
  // the user.
  inline std::string complete_extension(const std::string& name,
                                        const FileFilter& filter)
  {
    if (name.empty()) return name;
    if (has_extension(name)) return name;
    std::string ext = filter.default_extension();
    if (ext.empty()) return name;
    return name + "." + ext;
  }

  // ---- Directory listing model --------------------------------------------

  struct FileEntry
  {
    std::string name;
    bool        is_dir = false;
  };

  // Directories first, then files; each group ASCII case-insensitively by
  // name, with a case-sensitive tiebreak so the order is total (two names
  // differing only in case must not compare equal, or the sort is
  // unstable across implementations).
  inline bool file_entry_less(const FileEntry& a, const FileEntry& b)
  {
    if (a.is_dir != b.is_dir) return a.is_dir;
    const size_t n = std::min(a.name.size(), b.name.size());
    for (size_t i = 0; i < n; ++i) {
      char ca = ascii_lower(a.name[i]), cb = ascii_lower(b.name[i]);
      if (ca != cb) return ca < cb;
    }
    if (a.name.size() != b.name.size()) return a.name.size() < b.name.size();
    return a.name < b.name;
  }

  // Build the browser's visible row list from a raw directory read.
  //
  //   - "." and ".." are always dropped (the browser offers its own "up"
  //     affordance rather than a row that sorts unpredictably).
  //   - dot-files are dropped unless `show_hidden`.
  //   - directories are never filtered by file type - you have to be able
  //     to navigate INTO a folder to reach the files it holds, which is
  //     the bug the obvious "filter everything" implementation has.
  //   - `filter` == nullptr means no type filtering at all.
  inline std::vector<FileEntry> list_directory_view(
      const std::vector<FileEntry>& raw,
      const FileFilter* filter,
      bool show_hidden)
  {
    std::vector<FileEntry> out;
    out.reserve(raw.size());
    for (const auto& e : raw) {
      if (e.name.empty() || e.name == "." || e.name == "..") continue;
      if (!show_hidden && e.name[0] == '.') continue;
      if (!e.is_dir && filter && !filter_accepts(*filter, e.name)) continue;
      out.push_back(e);
    }
    std::sort(out.begin(), out.end(), file_entry_less);
    return out;
  }

} // namespace neui_detail
