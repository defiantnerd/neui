#pragma once

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// CF_HTML descriptor helper. The Windows "HTML Format" clipboard format is
// a UTF-8 byte stream prefixed with a small ASCII header that carries
// byte-offsets into the same buffer (StartHTML / EndHTML / StartFragment /
// EndFragment). The offsets are 10-ASCII-digit decimal numbers - we lay
// out the buffer with placeholder digits, then patch the placeholders
// with the real offsets once we know them.
//
// Reference: https://learn.microsoft.com/en-us/windows/win32/dataxchg/html-clipboard-format

namespace neui_detail
{
  inline UINT clipboard_cf_html_format()
  {
    static UINT cf = RegisterClipboardFormatW(L"HTML Format");
    return cf;
  }

  // Build the CF_HTML byte stream from raw HTML fragment bytes.
  // The fragment may be either a bare fragment or a full document; we
  // wrap it in <html><body> if a <body> tag isn't already present.
  inline std::vector<uint8_t> clipboard_encode_cf_html(const void* html_utf8,
                                                       uint32_t html_len)
  {
    const char* h = static_cast<const char*>(html_utf8);
    std::string fragment(h, h + html_len);

    // Detect whether the input already carries <body>; if not, wrap.
    bool has_body = false;
    for (size_t i = 0; i + 5 < fragment.size(); ++i) {
      if ((fragment[i] == '<') &&
          (fragment[i + 1] == 'b' || fragment[i + 1] == 'B') &&
          (fragment[i + 2] == 'o' || fragment[i + 2] == 'O') &&
          (fragment[i + 3] == 'd' || fragment[i + 3] == 'D') &&
          (fragment[i + 4] == 'y' || fragment[i + 4] == 'Y')) {
        has_body = true;
        break;
      }
    }

    std::string prefix, suffix;
    if (has_body) {
      prefix = "<!--StartFragment-->";
      suffix = "<!--EndFragment-->";
    } else {
      prefix = "<html><body><!--StartFragment-->";
      suffix = "<!--EndFragment--></body></html>";
    }

    // Header with 10-digit placeholder offsets. Each NNNNNNNNNN is zero-
    // padded so the total header length is constant and we can patch in
    // place after measuring.
    std::string header =
      "Version:0.9\r\n"
      "StartHTML:0000000000\r\n"
      "EndHTML:0000000000\r\n"
      "StartFragment:0000000000\r\n"
      "EndFragment:0000000000\r\n";

    std::string body = prefix + fragment + suffix;

    // Offsets (in bytes) into the final buffer.
    size_t start_html = header.size();
    size_t start_fragment = start_html + prefix.size();
    size_t end_fragment = start_fragment + fragment.size();
    size_t end_html = end_fragment + suffix.size();

    auto patch = [&](const char* key, size_t value) {
      // Find "key:0000000000\r\n" in the header and patch the 10 digits.
      size_t pos = header.find(key);
      if (pos == std::string::npos) return;
      pos += std::strlen(key);  // now at the ':' position
      // Skip past ':' then write 10 digits
      pos += 1;
      char digits[11];
      std::snprintf(digits, sizeof(digits), "%010zu", value);
      for (int i = 0; i < 10; ++i) header[pos + i] = digits[i];
    };

    patch("StartHTML",     start_html);
    patch("EndHTML",       end_html);
    patch("StartFragment", start_fragment);
    patch("EndFragment",   end_fragment);

    std::vector<uint8_t> out;
    out.reserve(header.size() + body.size());
    out.insert(out.end(), header.begin(), header.end());
    out.insert(out.end(), body.begin(), body.end());
    return out;
  }

  // Extract the fragment bytes from a CF_HTML buffer. Returns the bytes
  // between <!--StartFragment--> and <!--EndFragment-->, falling back to
  // the StartFragment/EndFragment byte offsets in the header.
  inline std::vector<uint8_t> clipboard_decode_cf_html(const void* data,
                                                       size_t length)
  {
    if (!data || length == 0) return {};
    const char* p = static_cast<const char*>(data);
    std::string text(p, p + length);

    auto find_header_offset = [&](const char* key) -> size_t {
      size_t pos = text.find(key);
      if (pos == std::string::npos) return SIZE_MAX;
      pos = text.find(':', pos);
      if (pos == std::string::npos) return SIZE_MAX;
      // Parse decimal digits after ':'
      ++pos;
      size_t value = 0;
      bool any = false;
      while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        value = value * 10 + (text[pos] - '0');
        any = true;
        ++pos;
      }
      return any ? value : SIZE_MAX;
    };

    size_t start = find_header_offset("StartFragment");
    size_t end   = find_header_offset("EndFragment");

    if (start != SIZE_MAX && end != SIZE_MAX && end >= start && end <= length) {
      std::vector<uint8_t> out;
      out.insert(out.end(), p + start, p + end);
      return out;
    }

    // Fallback: return whole buffer minus header (best-effort).
    size_t header_end = text.find("\r\n\r\n");
    if (header_end != std::string::npos) {
      header_end += 4;
      std::vector<uint8_t> out;
      out.insert(out.end(), p + header_end, p + length);
      return out;
    }

    return {};
  }

} // namespace neui_detail

#endif // _WIN32
