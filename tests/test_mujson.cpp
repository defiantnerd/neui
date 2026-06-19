// Tier-1 unit tests for the neui::mujson JSON-like parser (src/mujson.{h,cpp}).
//
// Migrated from the parser's stand-alone self-test onto the neui_test harness.
// mujson.cpp is compiled directly into the neui_tests target (the suite links
// no library), so these run on every platform incl. the null-platform CI build.

#include "neui_test.h"
#include "mujson.h"

#include <string>

using neui::mujson;

// --- helpers ---------------------------------------------------------------

namespace
{
  const mujson::node* find(const mujson::object_t& o, const std::string& key)
  {
    for (const auto& kv : o)
      if (kv.first == key) return &kv.second;
    return nullptr;
  }

  // Parse succeeded (note: empty {} is success with no error string).
  bool ok(const std::string& s)
  {
    auto r = mujson::parse(s);
    return !(r.empty() && *mujson::getLastError());
  }

  // Parse failed with exactly this error message.
  bool errIs(const std::string& s, const std::string& msg)
  {
    auto r = mujson::parse(s);
    return r.empty() && mujson::getLastError() == msg;
  }

  template <class T>
  bool is(const mujson::node* n) { return n && std::holds_alternative<T>(n->value); }
} // namespace

// --- tests -----------------------------------------------------------------

TEST_CASE("mujson: scalars and types")
{
  auto o = mujson::parse("{\"a\":\"hello\",\"b\":42}");
  CHECK(o.size() == 2);
  CHECK(is<std::string>(find(o, "a")) && std::get<std::string>(find(o, "a")->value) == "hello");
  CHECK(is<int>(find(o, "b")) && std::get<int>(find(o, "b")->value) == 42);

  // bare keys; int / non-int / leading zeros
  o = mujson::parse("{a:-7,b:12x,c:007}");
  CHECK(is<int>(find(o, "a")) && std::get<int>(find(o, "a")->value) == -7);
  CHECK(is<std::string>(find(o, "b")) && std::get<std::string>(find(o, "b")->value) == "12x");
  CHECK(is<int>(find(o, "c")) && std::get<int>(find(o, "c")->value) == 7);

  // floats vs ints; quoted vs malformed
  o = mujson::parse("{i:42,f:3.14,neg:-2.5,exp:1e3,whole:2.0,q:\"3.14\",bad:1.2.3}");
  CHECK(is<int>(find(o, "i")));
  CHECK(is<double>(find(o, "f")) && std::get<double>(find(o, "f")->value) == 3.14);
  CHECK(is<double>(find(o, "neg")));
  CHECK(is<double>(find(o, "exp")) && std::get<double>(find(o, "exp")->value) == 1000.0);
  CHECK(is<double>(find(o, "whole")));          // 2.0 stays a double
  CHECK(is<std::string>(find(o, "q")));         // quoted -> string
  CHECK(is<std::string>(find(o, "bad")));       // 1.2.3 -> string

  // bool / null literals, case-sensitive; quoted/partial stay strings
  o = mujson::parse("{a:true,b:false,c:null,d:\"true\",e:truex}");
  CHECK(is<bool>(find(o, "a")) && std::get<bool>(find(o, "a")->value) == true);
  CHECK(is<bool>(find(o, "b")) && std::get<bool>(find(o, "b")->value) == false);
  CHECK(is<std::monostate>(find(o, "c")));
  CHECK(is<std::string>(find(o, "d")));
  CHECK(is<std::string>(find(o, "e")));
}

TEST_CASE("mujson: nesting and arrays")
{
  auto o = mujson::parse("{user:{name:\"tk\",age:50},tags:[\"x\",\"y\",3]}");
  const auto* user = find(o, "user");
  CHECK(is<mujson::object_t>(user));
  const auto& uo = std::get<mujson::object_t>(user->value);
  CHECK(is<std::string>(find(uo, "name")) && std::get<std::string>(find(uo, "name")->value) == "tk");
  CHECK(is<int>(find(uo, "age")) && std::get<int>(find(uo, "age")->value) == 50);

  const auto* tags = find(o, "tags");
  CHECK(is<mujson::array_t>(tags));
  const auto& ta = std::get<mujson::array_t>(tags->value);
  CHECK(ta.size() == 3);
  CHECK(is<std::string>(&ta[0]) && is<int>(&ta[2]));

  // arrays of arrays
  o = mujson::parse("{list:[1,2,[3,4]]}");
  const auto& la = std::get<mujson::array_t>(find(o, "list")->value);
  CHECK(la.size() == 3 && is<mujson::array_t>(&la[2]));
  CHECK(std::get<mujson::array_t>(la[2].value).size() == 2);

  // empty containers
  o = mujson::parse("{o:{},a:[]}");
  CHECK(is<mujson::object_t>(find(o, "o")) && std::get<mujson::object_t>(find(o, "o")->value).empty());
  CHECK(is<mujson::array_t>(find(o, "a")) && std::get<mujson::array_t>(find(o, "a")->value).empty());

  // mixed array with all types
  o = mujson::parse("{m:[1,2.5,true,null,\"s\"]}");
  const auto& ma = std::get<mujson::array_t>(find(o, "m")->value);
  CHECK(ma.size() == 5);
  CHECK(is<int>(&ma[0]) && is<double>(&ma[1]) && is<bool>(&ma[2]) && is<std::monostate>(&ma[3]) && is<std::string>(&ma[4]));
}

TEST_CASE("mujson: whitespace and commas")
{
  CHECK(ok("{ outer : { inner : { deep : 7 } } , list : [1, 2, [3, 4]] }"));

  // single trailing comma tolerated (non-strict)
  CHECK(ok("{a:1,b:2,}"));
  CHECK(ok("{a:[1,2,3,],}"));

  // but malformed comma usage is rejected
  CHECK(errIs("{a:1,,b:2}", "key can only contain alphanumerics"));
  CHECK(errIs("{,a:1}", "key can only contain alphanumerics"));
  CHECK(errIs("{a:[,]}", "expected a value"));
}

TEST_CASE("mujson: comments")
{
  // line comments at various positions
  auto o = mujson::parse("{ // header\n a:1, // after a\n b:2 // after b\n }");
  CHECK(o.size() == 2);
  CHECK(is<int>(find(o, "a")) && std::get<int>(find(o, "a")->value) == 1);
  CHECK(is<int>(find(o, "b")) && std::get<int>(find(o, "b")->value) == 2);

  // block comments inline, incl. between key and colon and before the value
  o = mujson::parse("{ a /* k */ : /* v */ 1, /* mid */ b:2 /* tail */ }");
  CHECK(is<int>(find(o, "a")) && std::get<int>(find(o, "a")->value) == 1);
  CHECK(is<int>(find(o, "b")) && std::get<int>(find(o, "b")->value) == 2);

  // block comment spanning lines, and interspersed inside an array
  o = mujson::parse("{ list:[ 1, /* two */ 2,\n /* three\n   continued */ 3 ] }");
  const auto& la = std::get<mujson::array_t>(find(o, "list")->value);
  CHECK(la.size() == 3 && is<int>(&la[0]) && is<int>(&la[1]) && is<int>(&la[2]));

  // an object holding only a comment is an (empty) success
  CHECK(ok("{ /* nothing here */ }"));
  CHECK(ok("{ // just a line\n }"));

  // a comment after the closing brace is ignored
  CHECK(ok("{a:1} // trailing\n"));
  CHECK(ok("{a:1}/* trailing */"));

  // a lone '/' is NOT a comment: bare scalars may still contain a slash
  o = mujson::parse("{ a:1/2, b:foo/bar }");
  CHECK(is<std::string>(find(o, "a")) && std::get<std::string>(find(o, "a")->value) == "1/2");
  CHECK(is<std::string>(find(o, "b")) && std::get<std::string>(find(o, "b")->value) == "foo/bar");

  // an unterminated block comment is skipped to end -> reported as unexpected end
  CHECK(errIs("{ a:1 /* no end", "unexpected string termination"));
}

TEST_CASE("mujson: escapes and unicode")
{
  // named escapes inside a key and a value
  auto o = mujson::parse("{\"k\\tey\":\"a\\nb\"}");
  CHECK(o.size() == 1);
  CHECK(o[0].first == std::string("k\tey"));
  CHECK(std::get<std::string>(o[0].second.value) == std::string("a\nb"));

  // \u BMP -> UTF-8 (A, U+00E9 e-acute, U+20AC euro)
  o = mujson::parse("{k:\"\\u0041\\u00e9\\u20ac\"}");
  CHECK(std::get<std::string>(find(o, "k")->value) ==
        std::string("\x41\xc3\xa9\xe2\x82\xac"));

  // surrogate pair -> U+1F600 (4-byte UTF-8)
  o = mujson::parse("{k:\"\\uD83D\\uDE00\"}");
  CHECK(std::get<std::string>(find(o, "k")->value) ==
        std::string("\xf0\x9f\x98\x80"));

  // \u of a control char decodes to a single byte
  o = mujson::parse("{k:\"\\u0001\"}");
  CHECK(std::get<std::string>(find(o, "k")->value) == std::string(1, '\x01'));

  // unicode error paths
  CHECK(errIs("{k:\"\\u00G1\"}", "\\u must be followed by 4 hex digits"));
  CHECK(errIs("{k:\"\\uD83D x\"}", "invalid UTF-16 surrogate pair")); // lone high
  CHECK(errIs("{k:\"\\uDE00\"}", "invalid UTF-16 surrogate pair"));   // lone low
  CHECK(errIs("{k:\"\\q\"}", "invalid escape character after \\"));   // unknown escape
}

TEST_CASE("mujson: errors and depth")
{
  CHECK(errIs("not an object", "string does not start with curly braces"));
  CHECK(errIs("{a:1 b:2}", "expected ',' or '}'"));
  CHECK(errIs("{a:}", "expected a value"));
  CHECK(errIs("{a:[1,2}", "expected ',' or ']'"));
  CHECK(errIs("{a 1}", "expected ':' between key and value"));
  CHECK(errIs("{\"a\":\"x", "unexpected string termination"));

  // empty object is success, not an error
  CHECK(ok("{}"));
  CHECK(std::string(mujson::getLastError()).empty());

  // depth guard fires beyond kMaxDepth (128)
  std::string deep;
  for (int i = 0; i < 200; ++i) deep += "{a:";
  deep += "1";
  for (int i = 0; i < 200; ++i) deep += "}";
  CHECK(errIs(deep, "maximum nesting depth exceeded"));

  // a moderate depth still parses
  std::string okDepth;
  for (int i = 0; i < 50; ++i) okDepth += "{a:";
  okDepth += "1";
  for (int i = 0; i < 50; ++i) okDepth += "}";
  CHECK(ok(okDepth));
}

TEST_CASE("mujson: serialize")
{
  auto o = mujson::parse("{ name:\"tk\", age:50, ok:true, n:null, pi:3.14, whole:2.0, "
                         "list:[1,2.5,false,null,\"x\",], nested:{a:1,b:[true,]}, }");

  // compact form is strict JSON: quoted keys, no trailing commas
  CHECK(mujson::serialize(o) ==
        "{\"name\":\"tk\",\"age\":50,\"ok\":true,\"n\":null,\"pi\":3.14,"
        "\"whole\":2.0,\"list\":[1,2.5,false,null,\"x\"],"
        "\"nested\":{\"a\":1,\"b\":[true]}}");

  // empty containers
  CHECK(mujson::serialize(mujson::parse("{o:{},a:[]}")) == "{\"o\":{},\"a\":[]}");

  // pretty-print indents and newlines
  CHECK(mujson::serialize(mujson::parse("{a:1,b:[2,3]}"), 2) ==
        "{\n  \"a\": 1,\n  \"b\": [\n    2,\n    3\n  ]\n}");

  // control bytes escaped as \uXXXX
  mujson::object_t c;
  c.push_back({ "k", mujson::node{ std::string(1, '\x01') } });
  CHECK(mujson::serialize(c) == "{\"k\":\"\\u0001\"}");
}

TEST_CASE("mujson: roundtrip")
{
  // parse -> serialize -> parse -> serialize is stable
  auto o = mujson::parse("{a:1,b:2.0,c:true,d:null,e:\"x\",f:[1,{g:2}]}");
  std::string s1 = mujson::serialize(o);
  std::string s2 = mujson::serialize(mujson::parse(s1));
  CHECK(s1 == s2);

  // a string with quote/backslash/named-escapes/control byte round-trips exactly
  std::string raw = "q=\" b=\\ ";
  raw += '\n'; raw += '\t'; raw += '\r'; raw += '\b'; raw += '\f'; raw += '\x01';
  raw += " end";
  mujson::object_t obj;
  obj.push_back({ "k", mujson::node{ raw } });
  auto back = mujson::parse(mujson::serialize(obj));
  CHECK(!back.empty());
  CHECK(std::get<std::string>(back[0].second.value) == raw);

  // arbitrary UTF-8 (multi-byte) passes through unescaped and round-trips
  mujson::object_t u;
  u.push_back({ "k", mujson::node{ std::string("\xe2\x82\xac \xf0\x9f\x98\x80") } }); // "€ 😀"
  auto bu = mujson::parse(mujson::serialize(u));
  CHECK(std::get<std::string>(bu[0].second.value) == std::string("\xe2\x82\xac \xf0\x9f\x98\x80"));
}
