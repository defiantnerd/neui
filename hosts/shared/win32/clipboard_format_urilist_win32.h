#pragma once

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstdint>
#include <string>
#include <vector>

// CF_HDROP <-> text/uri-list translation. RFC 2483 specifies the wire
// format for text/uri-list: one URI per line terminated by CRLF, with
// '#' comments allowed. We use file:/// URIs with percent-encoded paths.
// CF_HDROP is the native Windows shell drop format: a DROPFILES struct
// followed by a double-null-terminated UTF-16 path list.

namespace neui_detail
{
  // Percent-encode all characters in `s` except RFC 3986 unreserved set
  // plus '/' (so path separators stay readable).
  inline std::string urilist_percent_encode_utf8(const std::string& s)
  {
    auto is_unreserved = [](unsigned char c) {
      return (c >= 'A' && c <= 'Z') ||
             (c >= 'a' && c <= 'z') ||
             (c >= '0' && c <= '9') ||
             c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
    };
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
      if (is_unreserved(c)) {
        out.push_back(static_cast<char>(c));
      } else {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%%%02X", c);
        out.append(buf);
      }
    }
    return out;
  }

  inline std::string urilist_percent_decode(const std::string& s)
  {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '%' && i + 2 < s.size()) {
        auto hex = [](char c) -> int {
          if (c >= '0' && c <= '9') return c - '0';
          if (c >= 'a' && c <= 'f') return c - 'a' + 10;
          if (c >= 'A' && c <= 'F') return c - 'A' + 10;
          return -1;
        };
        int h = hex(s[i + 1]);
        int l = hex(s[i + 2]);
        if (h >= 0 && l >= 0) {
          out.push_back(static_cast<char>((h << 4) | l));
          i += 2;
          continue;
        }
      }
      out.push_back(s[i]);
    }
    return out;
  }

  // Convert a Windows path (wchar_t) to a file:/// URI in UTF-8.
  //   C:\Users\me\Some File.txt  ->  file:///C:/Users/me/Some%20File.txt
  inline std::string urilist_path_to_uri(const wchar_t* wpath)
  {
    if (!wpath) return {};
    // Convert backslashes to forward slashes and UTF-16 -> UTF-8.
    int len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
                                   nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string utf8(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
                        utf8.data(), len, nullptr, nullptr);
    for (auto& c : utf8) if (c == '\\') c = '/';
    return std::string("file:///") + urilist_percent_encode_utf8(utf8);
  }

  // Convert a file:/// URI back to a native Windows path. Returns empty
  // wstring for non-file: schemes.
  inline std::wstring urilist_uri_to_path(const std::string& uri)
  {
    static const char prefix[] = "file://";
    if (uri.compare(0, sizeof(prefix) - 1, prefix) != 0) return {};
    size_t p = sizeof(prefix) - 1;
    // file:/// has empty host -> skip optional leading '/'
    if (p < uri.size() && uri[p] == '/') ++p;
    std::string decoded = urilist_percent_decode(uri.substr(p));
    for (auto& c : decoded) if (c == '/') c = '\\';
    int wn = MultiByteToWideChar(CP_UTF8, 0, decoded.data(),
                                  static_cast<int>(decoded.size()),
                                  nullptr, 0);
    std::wstring out(static_cast<size_t>(wn), L'\0');
    if (wn > 0) {
      MultiByteToWideChar(CP_UTF8, 0, decoded.data(),
                          static_cast<int>(decoded.size()),
                          out.data(), wn);
    }
    return out;
  }

  // Parse RFC 2483 text/uri-list bytes into a list of URI strings,
  // dropping comment lines (those starting with '#').
  inline std::vector<std::string> urilist_parse(const void* data, size_t length)
  {
    std::vector<std::string> out;
    if (!data || length == 0) return out;
    const char* p = static_cast<const char*>(data);
    size_t i = 0;
    while (i < length) {
      size_t end = i;
      while (end < length && p[end] != '\r' && p[end] != '\n') ++end;
      if (end > i) {
        std::string line(p + i, p + end);
        // Trim trailing whitespace.
        while (!line.empty() &&
               (line.back() == ' ' || line.back() == '\t'))
          line.pop_back();
        if (!line.empty() && line.front() != '#')
          out.push_back(std::move(line));
      }
      // Skip CR + LF, or either.
      while (end < length && (p[end] == '\r' || p[end] == '\n')) ++end;
      i = end;
    }
    return out;
  }

  // Build text/uri-list bytes from a list of native Windows paths.
  inline std::vector<uint8_t> urilist_encode_from_paths(
      const std::vector<std::wstring>& paths)
  {
    std::string text;
    for (auto& w : paths) {
      text += urilist_path_to_uri(w.c_str());
      text += "\r\n";
    }
    return std::vector<uint8_t>(text.begin(), text.end());
  }

  // Read CF_HDROP from the open clipboard and return text/uri-list bytes.
  // Returns empty vector on failure.
  inline std::vector<uint8_t> urilist_from_hdrop(HDROP hdrop)
  {
    if (!hdrop) return {};
    UINT n = DragQueryFileW(hdrop, 0xFFFFFFFFu, nullptr, 0);
    std::vector<std::wstring> paths;
    paths.reserve(n);
    for (UINT i = 0; i < n; ++i) {
      UINT need = DragQueryFileW(hdrop, i, nullptr, 0);
      std::wstring path(static_cast<size_t>(need), L'\0');
      DragQueryFileW(hdrop, i, path.data(), need + 1);
      paths.push_back(std::move(path));
    }
    return urilist_encode_from_paths(paths);
  }

  // Build a CF_HDROP HGLOBAL from text/uri-list bytes. Caller takes
  // ownership and must either GlobalFree or SetClipboardData transfer.
  inline HGLOBAL urilist_to_hdrop_global(const void* data, size_t length)
  {
    auto uris = urilist_parse(data, length);
    std::vector<std::wstring> paths;
    paths.reserve(uris.size());
    for (auto& u : uris) {
      auto wp = urilist_uri_to_path(u);
      if (!wp.empty()) paths.push_back(std::move(wp));
    }
    if (paths.empty()) return nullptr;

    // Layout: DROPFILES struct, then [path\0 path\0 ... \0]
    size_t chars = 1;  // trailing extra null
    for (auto& w : paths) chars += w.size() + 1;
    size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);

    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hg) return nullptr;
    auto* df = static_cast<DROPFILES*>(GlobalLock(hg));
    if (!df) { GlobalFree(hg); return nullptr; }
    df->pFiles = sizeof(DROPFILES);
    df->pt.x = 0;
    df->pt.y = 0;
    df->fNC = FALSE;
    df->fWide = TRUE;
    auto* w = reinterpret_cast<wchar_t*>(df + 1);
    for (auto& path : paths) {
      std::memcpy(w, path.data(), path.size() * sizeof(wchar_t));
      w += path.size();
      *w++ = L'\0';
    }
    *w = L'\0';
    GlobalUnlock(hg);
    return hg;
  }

} // namespace neui_detail

#endif // _WIN32
