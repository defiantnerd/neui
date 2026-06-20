/*
  mujson

  a minimal parser reading json-like strings.

  (c) 2021-2026 Timo Kaluza

  Recursive-descent implementation supporting nested objects and arrays.
*/

#include <charconv>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include "mujson.h"

namespace neui
{
  namespace
  {
    const char* lasterr = "";

    // Guards against stack overflow on deeply nested / hostile input.
    constexpr int kMaxDepth = 128;

    const char* const err_nocurlybraces       = "string does not start with curly braces";
    const char* const err_unexpected_end       = "unexpected string termination";
    const char* const err_invalid_chars_key    = "key can only contain alphanumerics";
    const char* const err_expected_colon       = "expected ':' between key and value";
    const char* const err_betweenitems_or_end  = "expected ',' or '}'";
    const char* const err_betweenelems_or_end  = "expected ',' or ']'";
    const char* const err_invalid_escape       = "invalid escape character after \\";
    const char* const err_bad_unicode_escape   = "\\u must be followed by 4 hex digits";
    const char* const err_bad_surrogate        = "invalid UTF-16 surrogate pair";
    const char* const err_expected_value       = "expected a value";
    const char* const err_max_depth            = "maximum nesting depth exceeded";

    // Set the error string and signal failure in one expression.
    inline bool fail(const char* msg) { lasterr = msg; return false; }

    // Skip whitespace and comments. Comments are a non-strict extension:
    // `//` runs to end-of-line, `/* ... */` spans (an unterminated block
    // comment is skipped to end-of-input, which the caller then reports as an
    // unexpected end). A lone '/' that begins neither form is left in place so
    // bare scalar values may still contain a slash. Note: a comment must be
    // separated from a bare (unquoted) scalar by whitespace or a structural
    // char - the scalar reader does not look ahead for comment starts.
    inline void skipWs(const char*& p)
    {
      for (;;)
      {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;

        if (p[0] == '/' && p[1] == '/')
        {
          p += 2;
          while (*p != 0 && *p != '\n') ++p;
          continue;
        }
        if (p[0] == '/' && p[1] == '*')
        {
          p += 2;
          while (*p != 0 && !(p[0] == '*' && p[1] == '/')) ++p;
          if (*p != 0) p += 2; // consume the closing */
          continue;
        }
        break;
      }
    }

    // Hex digit value, or -1 (also for the '\0' terminator).
    inline int hexValue(char c)
    {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    }

    // Read exactly 4 hex digits into `out`, advancing p. Stops safely at '\0'.
    bool readHex4(const char*& p, unsigned& out)
    {
      unsigned v = 0;
      for (int i = 0; i < 4; ++i)
      {
        int h = hexValue(*p);
        if (h < 0) return false;
        v = (v << 4) | static_cast<unsigned>(h);
        ++p;
      }
      out = v;
      return true;
    }

    // Append the UTF-8 encoding of a Unicode code point.
    void appendUtf8(std::string& out, unsigned cp)
    {
      if (cp <= 0x7F)
      {
        out.push_back(static_cast<char>(cp));
      }
      else if (cp <= 0x7FF)
      {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      }
      else if (cp <= 0xFFFF)
      {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      }
      else
      {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      }
    }

    // Decode the escape following a backslash and append the result (which may
    // be several UTF-8 bytes for \u). On entry *p is the char after '\'.
    bool decodeEscape(const char*& p, std::string& out)
    {
      char c = *p++;
      switch (c)
      {
      case 0:    return fail(err_unexpected_end);
      case 'b':  out.push_back(static_cast<char>(8));  return true;
      case 'f':  out.push_back(static_cast<char>(12)); return true;
      case 'n':  out.push_back(static_cast<char>(10)); return true;
      case 'r':  out.push_back(static_cast<char>(13)); return true;
      case 't':  out.push_back(static_cast<char>(9));  return true;
      case '\\':
      case '/':
      case '\"': out.push_back(c); return true;
      case 'u':
        {
          unsigned cp = 0;
          if (!readHex4(p, cp)) return fail(err_bad_unicode_escape);
          if (cp >= 0xD800 && cp <= 0xDBFF)
          {
            // High surrogate: must be followed by \u<low surrogate>.
            if (p[0] != '\\' || p[1] != 'u') return fail(err_bad_surrogate);
            p += 2;
            unsigned lo = 0;
            if (!readHex4(p, lo)) return fail(err_bad_unicode_escape);
            if (lo < 0xDC00 || lo > 0xDFFF) return fail(err_bad_surrogate);
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          else if (cp >= 0xDC00 && cp <= 0xDFFF)
          {
            return fail(err_bad_surrogate); // lone low surrogate
          }
          appendUtf8(out, cp);
          return true;
        }
      default:   return fail(err_invalid_escape);
      }
    }

    // Read a quoted token (used for both keys and string values).
    // On entry p points just past the opening quote; on success p points
    // just past the closing quote.
    bool parseQuoted(const char*& p, std::string& out)
    {
      for (;;)
      {
        char c = *p++;
        switch (c)
        {
        case 0:    return fail(err_unexpected_end);
        case '\"': return true;
        case '\\':
          if (!decodeEscape(p, out)) return false;
          break;
        default:
          out.push_back(c);
          break;
        }
      }
    }

    // Bareword key: [A-Za-z0-9_]+. On entry *p is the first (already-validated)
    // character.
    void parseBareKey(const char*& p, std::string& out)
    {
      for (;;)
      {
        unsigned char c = static_cast<unsigned char>(*p);
        if (std::isalnum(c) || c == '_') { out.push_back(static_cast<char>(c)); ++p; }
        else break;
      }
    }

    // Unquoted scalar value: read up to the next structural char / whitespace,
    // then emit an int if the whole token is a base-10 integer, else a string.
    bool parseScalar(const char*& p, mujson::node& out)
    {
      std::string tok;
      for (;;)
      {
        char c = *p;
        if (c == 0 || c == ',' || c == '}' || c == ']' ||
            c == ' ' || c == '\t' || c == '\r' || c == '\n')
          break;
        tok.push_back(c);
        ++p;
      }
      if (tok.empty()) return fail(err_expected_value);

      // Recognized bare-word literals (case-sensitive; quoted forms stay strings).
      if (tok == "true")  { out.value = true;             return true; }
      if (tok == "false") { out.value = false;            return true; }
      if (tok == "null")  { out.value = std::monostate{}; return true; }

      const char* b = tok.data();
      const char* e = b + tok.size();

      // Prefer a whole-token int; fall back to a whole-token double; else string.
      // The integer std::from_chars overload ships with libc++'s C++17 support
      // (no availability gate). The *floating-point* overload, however, is
      // annotated unavailable below macOS 13.3 / iOS 16.3 - calling it there is
      // a compile error, not a runtime fallback - so the double path uses the
      // portable std::strtod instead. tok is NUL-terminated (std::string) and
      // whitespace-free here; require the whole token consumed and a finite
      // result so bare words like "inf"/"nan" stay strings, and over/underflow
      // (errno == ERANGE) falls through to string - matching from_chars.
      int n = 0;
      auto ri = std::from_chars(b, e, n);
      if (ri.ec == std::errc() && ri.ptr == e) { out.value = n; return true; }

      errno = 0;
      char* dend = nullptr;
      double d = std::strtod(b, &dend);
      if (dend == e && errno == 0 && std::isfinite(d)) { out.value = d; return true; }

      out.value = std::move(tok);
      return true;
    }

    bool parseValue(const char*& p, mujson::node& out, int depth);

    // Object body. On entry p points just past '{'.
    bool parseObject(const char*& p, mujson::node& out, int depth)
    {
      mujson::object_t obj;
      for (;;)
      {
        skipWs(p);

        std::string key;
        char c = *p;
        // Closes an empty object, or tolerates a trailing comma (non-strict).
        if (c == '}') { ++p; out.value = std::move(obj); return true; }
        if (c == 0) return fail(err_unexpected_end);
        if (c == '\"')
        {
          ++p;
          if (!parseQuoted(p, key)) return false;
        }
        else if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
        {
          parseBareKey(p, key);
        }
        else
        {
          return fail(err_invalid_chars_key);
        }

        skipWs(p);
        if (*p != ':') return fail(err_expected_colon);
        ++p;

        mujson::node child;
        if (!parseValue(p, child, depth)) return false;
        obj.emplace_back(std::move(key), std::move(child));

        skipWs(p);
        c = *p++;
        if (c == ',') continue;
        if (c == '}') { out.value = std::move(obj); return true; }
        if (c == 0)   return fail(err_unexpected_end);
        return fail(err_betweenitems_or_end);
      }
    }

    // Array body. On entry p points just past '['.
    bool parseArray(const char*& p, mujson::node& out, int depth)
    {
      mujson::array_t arr;
      for (;;)
      {
        skipWs(p);
        // Closes an empty array, or tolerates a trailing comma (non-strict).
        if (*p == ']') { ++p; out.value = std::move(arr); return true; }

        mujson::node child;
        if (!parseValue(p, child, depth)) return false;
        arr.push_back(std::move(child));

        skipWs(p);
        char c = *p++;
        if (c == ',') continue;
        if (c == ']') { out.value = std::move(arr); return true; }
        if (c == 0)   return fail(err_unexpected_end);
        return fail(err_betweenelems_or_end);
      }
    }

    bool parseValue(const char*& p, mujson::node& out, int depth)
    {
      if (depth >= kMaxDepth) return fail(err_max_depth);
      skipWs(p);
      char c = *p;
      switch (c)
      {
      case 0:   return fail(err_unexpected_end);
      case '{': ++p; return parseObject(p, out, depth + 1);
      case '[': ++p; return parseArray(p, out, depth + 1);
      case '\"':
        {
          ++p;
          std::string s;
          if (!parseQuoted(p, s)) return false;
          out.value = std::move(s);
          return true;
        }
      default:
        return parseScalar(p, out);
      }
    }

    // ---- serialization -----------------------------------------------------

    // Append `s` as a quoted, escaped JSON string.
    void writeString(std::string& out, const std::string& s)
    {
      static const char* const hex = "0123456789abcdef";
      out.push_back('\"');
      for (char ch : s)
      {
        unsigned char c = static_cast<unsigned char>(ch);
        switch (c)
        {
        case '\"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
          if (c < 0x20)
          {
            out += "\\u00";
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
          }
          else
          {
            out.push_back(ch);
          }
          break;
        }
      }
      out.push_back('\"');
    }

    void writeDouble(std::string& out, double d)
    {
      // JSON has no NaN/Inf; emit null to keep the output valid.
      if (!std::isfinite(d)) { out += "null"; return; }
      // std::to_chars(double) is annotated unavailable below macOS 13.3 /
      // iOS 16.3 (compile error at a lower deployment target), so emit the
      // shortest %g precision (15..17 significant digits) that round-trips -
      // matching to_chars' shortest-representation output on every target.
      char buf[32];
      for (int prec = 15; prec <= 17; ++prec)
      {
        std::snprintf(buf, sizeof(buf), "%.*g", prec, d);
        if (std::strtod(buf, nullptr) == d) break;
      }
      std::string s(buf);
      // Keep the value typed as a double on re-parse (else "1000" reads as int).
      if (s.find_first_of(".eE") == std::string::npos) s += ".0";
      out += s;
    }

    void writeValue(std::string& out, const mujson::value_t& v, int indent, int depth);

    void writeObject(std::string& out, const mujson::object_t& obj, int indent, int depth)
    {
      if (obj.empty()) { out += "{}"; return; }
      const bool pretty = indent > 0;
      out.push_back('{');
      for (std::size_t i = 0; i < obj.size(); ++i)
      {
        if (i) out.push_back(',');
        if (pretty) { out.push_back('\n'); out.append(static_cast<std::size_t>(depth + 1) * indent, ' '); }
        writeString(out, obj[i].first);
        out.push_back(':');
        if (pretty) out.push_back(' ');
        writeValue(out, obj[i].second.value, indent, depth + 1);
      }
      if (pretty) { out.push_back('\n'); out.append(static_cast<std::size_t>(depth) * indent, ' '); }
      out.push_back('}');
    }

    void writeArray(std::string& out, const mujson::array_t& arr, int indent, int depth)
    {
      if (arr.empty()) { out += "[]"; return; }
      const bool pretty = indent > 0;
      out.push_back('[');
      for (std::size_t i = 0; i < arr.size(); ++i)
      {
        if (i) out.push_back(',');
        if (pretty) { out.push_back('\n'); out.append(static_cast<std::size_t>(depth + 1) * indent, ' '); }
        writeValue(out, arr[i].value, indent, depth + 1);
      }
      if (pretty) { out.push_back('\n'); out.append(static_cast<std::size_t>(depth) * indent, ' '); }
      out.push_back(']');
    }

    void writeValue(std::string& out, const mujson::value_t& v, int indent, int depth)
    {
      if (std::holds_alternative<std::monostate>(v))    out += "null";
      else if (std::holds_alternative<bool>(v))         out += std::get<bool>(v) ? "true" : "false";
      else if (std::holds_alternative<int>(v))          out += std::to_string(std::get<int>(v));
      else if (std::holds_alternative<double>(v))       writeDouble(out, std::get<double>(v));
      else if (std::holds_alternative<std::string>(v))  writeString(out, std::get<std::string>(v));
      else if (std::holds_alternative<mujson::object_t>(v)) writeObject(out, std::get<mujson::object_t>(v), indent, depth);
      else                                              writeArray(out, std::get<mujson::array_t>(v), indent, depth);
    }
  } // anonymous namespace

  mujson::object_t mujson::parse(const char* s)
  {
    lasterr = "";
    if (!s) { lasterr = err_nocurlybraces; return {}; }
    const char* p = s;
    skipWs(p);
    if (*p != '{') { lasterr = err_nocurlybraces; return {}; }
    ++p;

    node root;
    if (!parseObject(p, root, 1))
      return {};
    return std::get<object_t>(std::move(root.value));
  }

  const char* mujson::getLastError()
  {
    return lasterr;
  }

  std::string mujson::serialize(const object_t& obj, int indent)
  {
    std::string out;
    writeObject(out, obj, indent, 0);
    return out;
  }
}
