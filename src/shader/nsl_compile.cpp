// .nsl compiler: lexer, parser + type checker, lowering to scalar IR, and
// register allocation. Implements docs/shader-language.md.
//
// Lowering inlines every call and unrolls every loop, so a program becomes
// straight-line scalar code. Control flow becomes predication:
//   - if / else lowers both branches and merges every variable they changed
//     with a select on the condition;
//   - return, break and continue clear bits of per-function and per-loop
//     "alive" masks, and later assignments that must not reach those pixels
//     (out parameters, variables carried across loop iterations) are masked.
// Ints are compile-time values; only floats and bools reach the IR.

#include "nsl_ir.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

namespace neui {
namespace nsl {
namespace detail {
namespace {

// Limits, docs/shader-language.md section 13.
constexpr size_t k_max_source = 64 * 1024;
constexpr size_t k_max_ident = 63;
constexpr int k_max_depth = 128;
constexpr int k_max_functions = 64;
constexpr int k_max_uniforms = 32;
constexpr int k_max_uniform_components = 64;
constexpr int k_max_trip = 64;
constexpr int k_max_loop_nesting = 4;
constexpr int k_max_trip_product = 1024;
constexpr int k_max_ops = 8192;
constexpr int k_max_live = 256;
// Bounds on the lowering pass itself. Inlining multiplies work and nesting
// (f calls g twice, g calls h twice, ...), and CSE keeps the IR small while it
// happens, so neither the op limit nor the IR cap catches it.
constexpr int64_t k_max_lower_steps = int64_t(1) << 20;  // statements + expressions + if snapshots
constexpr int k_max_lower_depth = 512;                   // nesting after inlining
// Lowering stops long before the op limit could be checked, so a pathological
// program cannot exhaust memory first.
constexpr size_t k_max_ir_values = 200000;

struct CompileError {
  int line, col;
  std::string msg;
};

[[noreturn]] void fail(int line, int col, std::string msg) {
  throw CompileError{line, col, std::move(msg)};
}

std::string quote(const std::string& s) { return "'" + s + "'"; }

// ---------------------------------------------------------------------------
// Decimal -> binary32 with correct rounding (ties to even), independent of
// the C locale. A minimal unsigned big integer does the exact arithmetic.
// ---------------------------------------------------------------------------

struct Big {
  std::vector<uint32_t> w{0};  // little-endian base 2^32
};

void big_mul_add(Big& a, uint32_t m, uint32_t add) {
  uint64_t carry = add;
  for (uint32_t& x : a.w) {
    const uint64_t t = static_cast<uint64_t>(x) * m + carry;
    x = static_cast<uint32_t>(t);
    carry = t >> 32;
  }
  if (carry) a.w.push_back(static_cast<uint32_t>(carry));
}

void big_shl(Big& a, int bits_) {
  const int words = bits_ / 32, sh = bits_ % 32;
  if (sh) {
    uint32_t carry = 0;
    for (uint32_t& x : a.w) {
      const uint32_t nx = (x << sh) | carry;
      carry = x >> (32 - sh);
      x = nx;
    }
    if (carry) a.w.push_back(carry);
  }
  if (words) a.w.insert(a.w.begin(), static_cast<size_t>(words), 0u);
}

void big_trim(Big& a) {
  while (a.w.size() > 1 && a.w.back() == 0) a.w.pop_back();
}

int big_cmp(Big a, Big b) {
  big_trim(a);
  big_trim(b);
  if (a.w.size() != b.w.size()) return a.w.size() < b.w.size() ? -1 : 1;
  for (size_t i = a.w.size(); i-- > 0;)
    if (a.w[i] != b.w[i]) return a.w[i] < b.w[i] ? -1 : 1;
  return 0;
}

void big_sub(Big& a, const Big& b) {  // a -= b, requires a >= b
  int64_t borrow = 0;
  for (size_t i = 0; i < a.w.size(); ++i) {
    int64_t t = static_cast<int64_t>(a.w[i]) - borrow - (i < b.w.size() ? static_cast<int64_t>(b.w[i]) : 0);
    borrow = t < 0 ? 1 : 0;
    if (t < 0) t += (int64_t(1) << 32);
    a.w[i] = static_cast<uint32_t>(t);
  }
  big_trim(a);
}

int big_bitlen(Big a) {
  big_trim(a);
  int n = static_cast<int>(a.w.size() - 1) * 32;
  for (uint32_t top = a.w.back(); top; top >>= 1) ++n;
  return n;
}

// value = digits * 10^exp10. Returns false when the value overflows binary32.
bool decimal_to_float(const std::string& digits, int exp10, float& out) {
  const int len = static_cast<int>(digits.size());
  if (len == 0) { out = 0.0f; return true; }
  if (exp10 + len > 40) return false;               // >= 1e39 > FLT_MAX
  if (exp10 + len < -50) { out = 0.0f; return true; }  // < 1e-50, rounds to 0
  Big num, den;
  den.w[0] = 1;
  for (char ch : digits) big_mul_add(num, 10, static_cast<uint32_t>(ch - '0'));
  for (int i = 0; i < exp10; ++i) big_mul_add(num, 10, 0);
  for (int i = 0; i < -exp10; ++i) big_mul_add(den, 10, 0);
  // 2^e <= num / den < 2^(e + 1)
  int e = big_bitlen(num) - big_bitlen(den);
  {
    Big a = num, b = den;
    if (e >= 0) big_shl(b, e); else big_shl(a, -e);
    if (big_cmp(a, b) < 0) --e;
  }
  const int shift = e >= -126 ? 23 - e : 149;  // q = floor(num * 2^shift / den)
  Big a = num, b = den;
  if (shift >= 0) big_shl(a, shift); else big_shl(b, -shift);
  uint32_t q = 0;
  for (int bit = 24; bit >= 0; --bit) {
    Big t = b;
    big_shl(t, bit);
    if (big_cmp(a, t) >= 0) { big_sub(a, t); q |= 1u << bit; }
  }
  big_shl(a, 1);  // compare the remainder with half the divisor
  const int c = big_cmp(a, b);
  if (c > 0 || (c == 0 && (q & 1u))) ++q;
  if (e >= -126) {
    if (q == (1u << 24)) { q >>= 1; ++e; }
    if (e > 127) return false;
    out = from_bits((static_cast<uint32_t>(e + 127) << 23) | (q & 0x7fffffu));
  } else {
    out = from_bits(q);  // subnormal; rounding up to 2^23 gives FLT_MIN exactly
  }
  return true;
}

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

enum Tok : uint8_t {
  T_END, T_IDENT, T_INT, T_FLOAT,
  T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_COMMA, T_SEMI, T_DOT, T_QUESTION, T_COLON,
  T_PLUS, T_MINUS, T_STAR, T_SLASH, T_BANG,
  T_LT, T_GT, T_LE, T_GE, T_EQEQ, T_NE, T_ANDAND, T_OROR, T_XORXOR,
  T_ASSIGN, T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ, T_PLUSPLUS, T_MINUSMINUS,
  T_UNIFORM, T_CONST, T_KW_FLOAT, T_KW_INT, T_KW_BOOL, T_KW_VEC2, T_KW_VEC3, T_KW_VEC4,
  T_KW_VOID, T_TRUE, T_FALSE, T_IF, T_ELSE, T_FOR, T_BREAK, T_CONTINUE, T_RETURN,
  T_IN, T_OUT, T_INOUT, T_PRECISION, T_LOWP, T_MEDIUMP, T_HIGHP,
};

struct Token {
  Tok t = T_END;
  int line = 1, col = 1;
  std::string text;  // identifiers, and every token's spelling for messages
  int64_t ival = 0;
  float fval = 0.0f;
};

const char* const k_reserved_words[] = {
  "while", "do", "switch", "case", "default", "discard", "struct", "attribute", "varying",
  "layout", "centroid", "flat", "smooth", "invariant", "mat2", "mat3", "mat4", "mat2x2",
  "mat2x3", "mat2x4", "mat3x2", "mat3x3", "mat3x4", "mat4x2", "mat4x3", "mat4x4", "ivec2",
  "ivec3", "ivec4", "bvec2", "bvec3", "bvec4", "uvec2", "uvec3", "uvec4", "uint", "double",
  "dvec2", "dvec3", "dvec4", "half", "sampler2D", "sampler3D", "samplerCube", "texture",
  "texture2D", "textureCube", "asm", "class", "union", "enum", "typedef", "template", "this",
  "goto", "inline", "noinline", "volatile", "public", "static", "extern", "interface", "long",
  "short", "unsigned", "sizeof", "cast", "namespace", "using"};

struct Keyword { const char* word; Tok t; };
const Keyword k_keywords[] = {
  {"uniform", T_UNIFORM}, {"const", T_CONST}, {"float", T_KW_FLOAT}, {"int", T_KW_INT},
  {"bool", T_KW_BOOL}, {"vec2", T_KW_VEC2}, {"vec3", T_KW_VEC3}, {"vec4", T_KW_VEC4},
  {"void", T_KW_VOID}, {"true", T_TRUE}, {"false", T_FALSE}, {"if", T_IF}, {"else", T_ELSE},
  {"for", T_FOR}, {"break", T_BREAK}, {"continue", T_CONTINUE}, {"return", T_RETURN},
  {"in", T_IN}, {"out", T_OUT}, {"inout", T_INOUT}, {"precision", T_PRECISION},
  {"lowp", T_LOWP}, {"mediump", T_MEDIUMP}, {"highp", T_HIGHP}};

bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alnum(char c) { return is_alpha(c) || is_digit(c); }

class Lexer {
 public:
  Lexer(const char* s, size_t n) : s_(s), n_(n) {}

  std::vector<Token> run(uint32_t& flag_bits) {
    std::vector<Token> out;
    if (n_ >= 3 && static_cast<unsigned char>(s_[0]) == 0xEF &&
        static_cast<unsigned char>(s_[1]) == 0xBB && static_cast<unsigned char>(s_[2]) == 0xBF)
      i_ = 3;
    for (;;) {
      skip_space_and_comments(flag_bits);
      Token t;
      t.line = line_;
      t.col = col_;
      if (i_ >= n_) { t.t = T_END; t.text = "end of file"; out.push_back(t); return out; }
      const char c = s_[i_];
      line_start_ = false;
      if (is_alpha(c)) ident(t);
      else if (is_digit(c) || (c == '.' && i_ + 1 < n_ && is_digit(s_[i_ + 1]))) number(t);
      else punct(t);
      out.push_back(std::move(t));
    }
  }

 private:
  char at(size_t k) const { return i_ + k < n_ ? s_[i_ + k] : '\0'; }

  void adv() {
    const char c = s_[i_];
    if (c == '\r' && at(1) == '\n') { i_ += 2; ++line_; col_ = 1; line_start_ = true; }
    else if (c == '\r' || c == '\n') { ++i_; ++line_; col_ = 1; line_start_ = true; }
    else { ++i_; ++col_; }
  }

  void skip_space_and_comments(uint32_t& flag_bits) {
    while (i_ < n_) {
      const char c = s_[i_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') { adv(); continue; }
      if (c == '/' && at(1) == '/') {
        while (i_ < n_ && s_[i_] != '\n' && s_[i_] != '\r') adv();
        continue;
      }
      if (c == '/' && at(1) == '*') {
        const int l = line_, cl = col_;
        adv(); adv();
        while (i_ < n_ && !(s_[i_] == '*' && at(1) == '/')) adv();
        if (i_ >= n_) fail(l, cl, "unterminated comment");
        adv(); adv();
        continue;
      }
      if (c == '#' && line_start_) { directive(flag_bits); continue; }
      if (static_cast<unsigned char>(c) >= 0x80) fail(line_, col_, "non-ASCII character outside a comment");
      return;
    }
  }

  // "#pragma neui <name>" is the only directive.
  void directive(uint32_t& flag_bits) {
    const int l = line_, cl = col_;
    std::vector<std::string> words;
    adv();
    while (i_ < n_ && s_[i_] != '\n' && s_[i_] != '\r') {
      if (s_[i_] == '/' && (at(1) == '/' || at(1) == '*')) break;
      if (s_[i_] == ' ' || s_[i_] == '\t') { adv(); continue; }
      std::string w;
      while (i_ < n_ && s_[i_] != ' ' && s_[i_] != '\t' && s_[i_] != '\n' && s_[i_] != '\r') {
        w += s_[i_];
        adv();
      }
      words.push_back(w);
    }
    if (words.size() < 2 || words[0] != "pragma" || words[1] != "neui")
      fail(l, cl, "the preprocessor is not supported; the only directive is '#pragma neui <name>'");
    if (words.size() != 3) fail(l, cl, "'#pragma neui' takes one name");
    if (words[2] == "dither") flag_bits |= NSL_DITHER;
    else if (words[2] == "cpu_only") flag_bits |= NSL_CPU_ONLY;
    else fail(l, cl, "unknown pragma " + quote(words[2]));
  }

  void ident(Token& t) {
    while (i_ < n_ && is_alnum(s_[i_])) { t.text += s_[i_]; adv(); }
    if (t.text.size() > k_max_ident)
      fail(t.line, t.col, "identifier is longer than " + std::to_string(k_max_ident) + " characters");
    for (const Keyword& k : k_keywords)
      if (t.text == k.word) { t.t = k.t; return; }
    for (const char* r : k_reserved_words)
      if (t.text == r) fail(t.line, t.col, quote(t.text) + " is not supported in .nsl v1");
    if (t.text.compare(0, 3, "gl_") == 0)
      fail(t.line, t.col, quote(t.text) + " is reserved; main returns the colour and takes the position as its parameter");
    if (t.text.find("__") != std::string::npos)
      fail(t.line, t.col, quote(t.text) + " is reserved (names containing __)");
    t.t = T_IDENT;
  }

  void number(Token& t) {
    std::string int_part, frac_part;
    bool is_float = false;
    int exp = 0;
    while (i_ < n_ && is_digit(s_[i_])) { int_part += s_[i_]; adv(); }
    if (at(0) == '.') {
      is_float = true;
      adv();
      while (i_ < n_ && is_digit(s_[i_])) { frac_part += s_[i_]; adv(); }
    }
    if (at(0) == 'e' || at(0) == 'E') {
      const size_t k = (at(1) == '+' || at(1) == '-') ? 2 : 1;
      if (is_digit(at(k))) {
        is_float = true;
        adv();
        bool neg = false;
        if (at(0) == '+' || at(0) == '-') { neg = at(0) == '-'; adv(); }
        while (i_ < n_ && is_digit(s_[i_])) {
          if (exp < 100000) exp = exp * 10 + (s_[i_] - '0');
          adv();
        }
        if (neg) exp = -exp;
      }
    }
    if (is_float && (at(0) == 'f' || at(0) == 'F')) adv();
    if (is_alnum(at(0)) || at(0) == '.') fail(t.line, t.col, "malformed number");
    t.text = int_part + (is_float ? "." + frac_part : "");
    if (!is_float) {
      int64_t v = 0;
      for (char ch : int_part) {
        v = v * 10 + (ch - '0');
        if (v > 2147483647) fail(t.line, t.col, "integer literal is too large");
      }
      t.t = T_INT;
      t.ival = v;
      return;
    }
    std::string digits = int_part + frac_part;
    int exp10 = exp - static_cast<int>(frac_part.size());
    size_t lead = 0;
    while (lead < digits.size() && digits[lead] == '0') ++lead;
    digits.erase(0, lead);
    while (!digits.empty() && digits.back() == '0') { digits.pop_back(); ++exp10; }
    if (digits.size() > 64) fail(t.line, t.col, "float literal has more than 64 significant digits");
    if (!decimal_to_float(digits, exp10, t.fval)) fail(t.line, t.col, "float literal is too large");
    t.t = T_FLOAT;
  }

  void punct(Token& t) {
    struct P { const char* s; Tok t; };
    static const P two[] = {
      {"<=", T_LE}, {">=", T_GE}, {"==", T_EQEQ}, {"!=", T_NE}, {"&&", T_ANDAND}, {"||", T_OROR},
      {"^^", T_XORXOR}, {"+=", T_PLUSEQ}, {"-=", T_MINUSEQ}, {"*=", T_STAREQ}, {"/=", T_SLASHEQ},
      {"++", T_PLUSPLUS}, {"--", T_MINUSMINUS}};
    static const P one[] = {
      {"(", T_LPAREN}, {")", T_RPAREN}, {"{", T_LBRACE}, {"}", T_RBRACE}, {",", T_COMMA},
      {";", T_SEMI}, {".", T_DOT}, {"?", T_QUESTION}, {":", T_COLON}, {"+", T_PLUS},
      {"-", T_MINUS}, {"*", T_STAR}, {"/", T_SLASH}, {"!", T_BANG}, {"<", T_LT}, {">", T_GT},
      {"=", T_ASSIGN}};
    for (const P& p : two)
      if (s_[i_] == p.s[0] && at(1) == p.s[1]) { t.t = p.t; t.text = p.s; adv(); adv(); return; }
    for (const P& p : one)
      if (s_[i_] == p.s[0]) { t.t = p.t; t.text = p.s; adv(); return; }
    const char c = s_[i_];
    if (c == '%') fail(t.line, t.col, "'%' is not supported in .nsl v1; use mod()");
    if (c == '&' || c == '|' || c == '^' || c == '~')
      fail(t.line, t.col, std::string("bitwise operator '") + c + "' is not supported in .nsl v1");
    if (c == '[' || c == ']') fail(t.line, t.col, "arrays are not supported in .nsl v1");
    fail(t.line, t.col, std::string("unexpected character '") + c + "'");
  }

  const char* s_;
  size_t n_;
  size_t i_ = 0;
  int line_ = 1, col_ = 1;
  bool line_start_ = true;
};

// ---------------------------------------------------------------------------
// Types and syntax tree
// ---------------------------------------------------------------------------

enum class Ty : uint8_t { Void, Bool, Int, Float, Vec2, Vec3, Vec4 };

int ty_comps(Ty t) {
  switch (t) {
    case Ty::Vec2: return 2;
    case Ty::Vec3: return 3;
    case Ty::Vec4: return 4;
    case Ty::Void: return 0;
    default: return 1;
  }
}
bool ty_floaty(Ty t) { return t == Ty::Float || t == Ty::Vec2 || t == Ty::Vec3 || t == Ty::Vec4; }
bool ty_vec(Ty t) { return t == Ty::Vec2 || t == Ty::Vec3 || t == Ty::Vec4; }
Ty vec_of(int n) { return n == 1 ? Ty::Float : n == 2 ? Ty::Vec2 : n == 3 ? Ty::Vec3 : Ty::Vec4; }
const char* ty_name(Ty t) {
  switch (t) {
    case Ty::Void: return "void";
    case Ty::Bool: return "bool";
    case Ty::Int: return "int";
    case Ty::Float: return "float";
    case Ty::Vec2: return "vec2";
    case Ty::Vec3: return "vec3";
    default: return "vec4";
  }
}
// An int converts to float implicitly; nothing else converts.
bool converts_to(Ty from, Ty to) { return from == to || (from == Ty::Int && to == Ty::Float); }

enum Qual : uint8_t { Q_IN, Q_OUT, Q_INOUT };

struct Expr;
struct Function;

struct VarDecl {
  enum Kind : uint8_t { LOCAL, PARAM, UNIFORM, GLOBAL_CONST, LOOP_COUNTER, RESERVED } kind = LOCAL;
  std::string name;
  Ty type = Ty::Float;
  int line = 0, col = 0;
  bool is_const = false;
  Qual qual = Q_IN;
  int slot = 0;       // UNIFORM: first value index; RESERVED: which
  const Expr* init = nullptr;  // GLOBAL_CONST / UNIFORM default
};

enum Builtin : uint8_t {
  // genType -> genType
  B_ABS, B_SIGN, B_FLOOR, B_CEIL, B_FRACT, B_SQRT, B_INVSQRT, B_SIN, B_COS, B_TAN, B_ASIN,
  B_ACOS, B_EXP, B_LOG, B_EXP2, B_LOG2, B_RADIANS, B_DEGREES, B_SATURATE,
  // atan takes 1 or 2 arguments
  B_ATAN,
  // (genType, genType) -> genType
  B_MOD, B_MIN, B_MAX, B_STEP, B_POW,
  // (genType, genType, genType) -> genType
  B_CLAMP, B_MIX, B_SMOOTHSTEP,
  // geometric and noise
  B_LENGTH, B_DISTANCE, B_DOT, B_NORMALIZE, B_CROSS, B_REFLECT, B_HASH, B_NOISE,
  B_NONE
};

struct BuiltinName { const char* name; Builtin b; };
const BuiltinName k_builtins[] = {
  {"abs", B_ABS}, {"sign", B_SIGN}, {"floor", B_FLOOR}, {"ceil", B_CEIL}, {"fract", B_FRACT},
  {"sqrt", B_SQRT}, {"inversesqrt", B_INVSQRT}, {"sin", B_SIN}, {"cos", B_COS}, {"tan", B_TAN},
  {"asin", B_ASIN}, {"acos", B_ACOS}, {"exp", B_EXP}, {"log", B_LOG}, {"exp2", B_EXP2},
  {"log2", B_LOG2}, {"radians", B_RADIANS}, {"degrees", B_DEGREES}, {"saturate", B_SATURATE},
  {"atan", B_ATAN}, {"mod", B_MOD}, {"min", B_MIN}, {"max", B_MAX}, {"step", B_STEP},
  {"pow", B_POW}, {"clamp", B_CLAMP}, {"mix", B_MIX}, {"smoothstep", B_SMOOTHSTEP},
  {"length", B_LENGTH}, {"distance", B_DISTANCE}, {"dot", B_DOT}, {"normalize", B_NORMALIZE},
  {"cross", B_CROSS}, {"reflect", B_REFLECT}, {"neui_hash", B_HASH}, {"neui_noise", B_NOISE}};

Builtin find_builtin(const std::string& name) {
  for (const BuiltinName& b : k_builtins)
    if (name == b.name) return b.b;
  return B_NONE;
}

struct Expr {
  enum Kind : uint8_t { INT, FLOAT, BOOL, VAR, CALL, CTOR, SWIZZLE, UNARY, BINARY, TERNARY } k = INT;
  int line = 0, col = 0;
  Ty type = Ty::Void;
  int64_t ival = 0;
  float fval = 0.0f;
  const VarDecl* var = nullptr;
  Builtin builtin = B_NONE;
  const Function* fn = nullptr;
  Tok op = T_END;
  uint8_t swz[4] = {0, 0, 0, 0};
  int swz_n = 0;
  std::vector<std::unique_ptr<Expr>> args;
};

struct Stmt {
  enum Kind : uint8_t { BLOCK, DECL, ASSIGN, CALL, IF, FOR, BREAK, CONTINUE, RETURN, EMPTY } k = EMPTY;
  int line = 0, col = 0;
  std::vector<std::unique_ptr<Stmt>> body;                         // BLOCK
  std::vector<std::pair<const VarDecl*, std::unique_ptr<Expr>>> decls;  // DECL
  std::unique_ptr<Expr> target, value;                             // ASSIGN; value: RETURN, CALL
  Tok assign_op = T_ASSIGN;
  std::unique_ptr<Expr> cond;                                      // IF
  std::unique_ptr<Stmt> then_s, else_s;                            // IF; then_s: FOR body
  const VarDecl* counter = nullptr;                                // FOR
  std::unique_ptr<Expr> for_init, for_bound, for_step;
  Tok for_cmp = T_LT;
  int for_dir = 1;
};

struct Function {
  std::string name;
  Ty ret = Ty::Void;
  int line = 0, col = 0;
  std::vector<const VarDecl*> params;
  std::unique_ptr<Stmt> body;
};

// ---------------------------------------------------------------------------
// Parser + type checker (one pass: every name is declared before use)
// ---------------------------------------------------------------------------

class Parser {
 public:
  Parser(std::vector<Token> toks, uint32_t flag_bits) : toks_(std::move(toks)), flags_(flag_bits) {
    const char* names[RES_COUNT] = {nullptr, nullptr, "neui_scale", "neui_px", "neui_time"};
    // neui_size is one vec2 over RES_SIZE_X / RES_SIZE_Y.
    add_reserved("neui_size", Ty::Vec2, RES_SIZE_X);
    for (int r = RES_SCALE; r < RES_COUNT; ++r) add_reserved(names[r], Ty::Float, r);
  }

  void parse_program() {
    while (peek().t != T_END) parse_global();
    if (!main_)
      fail(peek().line, peek().col, "no 'vec4 main(vec2 p)' is defined");
  }

  const Function* main_fn() const { return main_; }
  const std::vector<const VarDecl*>& uniforms() const { return uniform_list_; }
  const std::vector<const VarDecl*>& global_consts() const { return const_list_; }
  int uniform_values() const { return uniform_values_; }
  uint32_t flag_bits() const { return flags_; }

 private:
  // --- tokens ---
  const Token& peek(int k = 0) const {
    const size_t i = std::min(pos_ + static_cast<size_t>(k), toks_.size() - 1);
    return toks_[i];
  }
  const Token& next() {
    const Token& t = toks_[pos_];
    if (pos_ + 1 < toks_.size()) ++pos_;
    return t;
  }
  bool accept(Tok t) {
    if (peek().t != t) return false;
    next();
    return true;
  }
  const Token& expect(Tok t, const char* what) {
    if (peek().t != t) fail(peek().line, peek().col, std::string("expected ") + what + ", found " + quote(peek().text));
    return next();
  }

  struct DepthGuard {
    Parser& p;
    DepthGuard(Parser& parser, const Token& at) : p(parser) {
      if (++p.depth_ > k_max_depth)
        fail(at.line, at.col, "nesting is deeper than " + std::to_string(k_max_depth));
    }
    ~DepthGuard() { --p.depth_; }
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;
  };

  // --- declarations ---
  VarDecl* new_var() {
    vars_.push_back(std::make_unique<VarDecl>());
    return vars_.back().get();
  }

  void add_reserved(const char* name, Ty t, int which) {
    VarDecl* v = new_var();
    v->kind = VarDecl::RESERVED;
    v->name = name;
    v->type = t;
    v->slot = which;
    globals_[name] = v;
  }

  void check_new_name(const Token& t) {
    if (t.text.compare(0, 5, "neui_") == 0)
      fail(t.line, t.col, quote(t.text) + " is reserved (names starting with neui_)");
    if (find_builtin(t.text) != B_NONE)
      fail(t.line, t.col, quote(t.text) + " is a built-in function");
  }

  void declare_local(VarDecl* v, const Token& at) {
    check_new_name(at);
    if (functions_.count(v->name)) fail(at.line, at.col, quote(v->name) + " is a function");
    auto& scope = scopes_.back();
    if (scope.count(v->name)) fail(at.line, at.col, quote(v->name) + " is already declared");
    scope[v->name] = v;
  }

  void declare_global(VarDecl* v, const Token& at) {
    check_new_name(at);
    if (globals_.count(v->name) || functions_.count(v->name))
      fail(at.line, at.col, quote(v->name) + " is already declared");
    globals_[v->name] = v;
  }

  const VarDecl* lookup(const std::string& name) const {
    for (size_t i = scopes_.size(); i-- > 0;) {
      auto it = scopes_[i].find(name);
      if (it != scopes_[i].end()) return it->second;
    }
    auto it = globals_.find(name);
    return it == globals_.end() ? nullptr : it->second;
  }

  static bool is_type_tok(Tok t) {
    return t == T_KW_FLOAT || t == T_KW_INT || t == T_KW_BOOL || t == T_KW_VEC2 || t == T_KW_VEC3 ||
           t == T_KW_VEC4;
  }
  static Ty tok_type(Tok t) {
    switch (t) {
      case T_KW_INT: return Ty::Int;
      case T_KW_BOOL: return Ty::Bool;
      case T_KW_VEC2: return Ty::Vec2;
      case T_KW_VEC3: return Ty::Vec3;
      case T_KW_VEC4: return Ty::Vec4;
      case T_KW_VOID: return Ty::Void;
      default: return Ty::Float;
    }
  }
  void skip_precision() { while (peek().t == T_LOWP || peek().t == T_MEDIUMP || peek().t == T_HIGHP) next(); }

  // --- globals ---
  void parse_global() {
    const Token& t = peek();
    if (t.t == T_PRECISION) {
      next();
      skip_precision();
      expect(T_KW_FLOAT, "'float'");
      expect(T_SEMI, "';'");
      return;
    }
    if (t.t == T_UNIFORM) { parse_uniform(); return; }
    if (t.t == T_CONST) { parse_global_const(); return; }
    skip_precision();
    if (!is_type_tok(peek().t) && peek().t != T_KW_VOID)
      fail(peek().line, peek().col, "expected a declaration, found " + quote(peek().text));
    const Token& type_tok = next();
    const Token& name = expect(T_IDENT, "a name");
    if (peek().t != T_LPAREN)
      fail(name.line, name.col, "global variables must be 'const' or 'uniform'");
    parse_function(type_tok, name);
  }

  void parse_uniform() {
    next();
    skip_precision();
    const Token& type_tok = next();
    if (!(type_tok.t == T_KW_FLOAT || type_tok.t == T_KW_VEC2 || type_tok.t == T_KW_VEC3 || type_tok.t == T_KW_VEC4))
      fail(type_tok.line, type_tok.col, "uniforms are float, vec2, vec3 or vec4");
    const Token& name = expect(T_IDENT, "a uniform name");
    VarDecl* v = new_var();
    v->kind = VarDecl::UNIFORM;
    v->name = name.text;
    v->type = tok_type(type_tok.t);
    v->line = name.line;
    v->col = name.col;
    v->slot = uniform_values_;
    declare_global(v, name);
    if (static_cast<int>(uniform_list_.size()) >= k_max_uniforms)
      fail(name.line, name.col, "more than " + std::to_string(k_max_uniforms) + " uniforms");
    uniform_values_ += ty_comps(v->type);
    if (uniform_values_ > k_max_uniform_components)
      fail(name.line, name.col, "uniforms need more than " + std::to_string(k_max_uniform_components) + " floats");
    if (accept(T_ASSIGN)) {
      const Token& at = peek();
      std::unique_ptr<Expr> e = parse_expr();
      if (!converts_to(e->type, v->type))
        fail(at.line, at.col, "default of " + quote(v->name) + " is " + ty_name(e->type) + ", expected " + ty_name(v->type));
      v->init = e.get();
      owned_exprs_.push_back(std::move(e));
    }
    expect(T_SEMI, "';'");
    uniform_list_.push_back(v);
  }

  void parse_global_const() {
    next();
    skip_precision();
    const Token& type_tok = next();
    if (!is_type_tok(type_tok.t)) fail(type_tok.line, type_tok.col, "expected a type after 'const'");
    do {
      const Token& name = expect(T_IDENT, "a name");
      VarDecl* v = new_var();
      v->kind = VarDecl::GLOBAL_CONST;
      v->is_const = true;
      v->name = name.text;
      v->type = tok_type(type_tok.t);
      v->line = name.line;
      v->col = name.col;
      expect(T_ASSIGN, "'=' (a const needs a value)");
      const Token& at = peek();
      std::unique_ptr<Expr> e = parse_expr();
      if (!converts_to(e->type, v->type))
        fail(at.line, at.col, quote(v->name) + " is " + ty_name(v->type) + ", but its value is " + ty_name(e->type));
      v->init = e.get();
      owned_exprs_.push_back(std::move(e));
      declare_global(v, name);
      const_list_.push_back(v);
    } while (accept(T_COMMA));
    expect(T_SEMI, "';'");
  }

  void parse_function(const Token& type_tok, const Token& name) {
    check_new_name(name);
    if (functions_.count(name.text) || globals_.count(name.text))
      fail(name.line, name.col, quote(name.text) + " is already declared (user functions cannot be overloaded)");
    if (static_cast<int>(functions_.size()) >= k_max_functions)
      fail(name.line, name.col, "more than " + std::to_string(k_max_functions) + " functions");
    auto fn = std::make_unique<Function>();
    fn->name = name.text;
    fn->ret = tok_type(type_tok.t);
    fn->line = name.line;
    fn->col = name.col;
    if (fn->ret == Ty::Int) fail(type_tok.line, type_tok.col, "functions cannot return int");
    expect(T_LPAREN, "'('");
    scopes_.emplace_back();
    if (!(peek().t == T_KW_VOID && peek(1).t == T_RPAREN) && peek().t != T_RPAREN) {
      do {
        Qual q = Q_IN;
        if (accept(T_IN)) q = Q_IN;
        else if (accept(T_OUT)) q = Q_OUT;
        else if (accept(T_INOUT)) q = Q_INOUT;
        skip_precision();
        const Token& pt = next();
        if (!is_type_tok(pt.t)) fail(pt.line, pt.col, "expected a parameter type, found " + quote(pt.text));
        const Token& pn = expect(T_IDENT, "a parameter name");
        VarDecl* v = new_var();
        v->kind = VarDecl::PARAM;
        v->name = pn.text;
        v->type = tok_type(pt.t);
        v->qual = q;
        v->line = pn.line;
        v->col = pn.col;
        if (v->type == Ty::Int && q != Q_IN) fail(pn.line, pn.col, "int parameters must be 'in'");
        declare_local(v, pn);
        fn->params.push_back(v);
      } while (accept(T_COMMA));
    } else {
      accept(T_KW_VOID);
    }
    expect(T_RPAREN, "')'");
    const bool is_main = fn->name == "main";
    if (is_main && !(fn->ret == Ty::Vec4 && fn->params.size() == 1 && fn->params[0]->type == Ty::Vec2 &&
                     fn->params[0]->qual == Q_IN))
      fail(name.line, name.col, "main must be declared as 'vec4 main(vec2 p)'");
    Function* raw = fn.get();
    functions_[raw->name] = std::move(fn);
    current_fn_ = raw;
    if (peek().t != T_LBRACE) fail(peek().line, peek().col, "expected the function body");
    raw->body = parse_block(false);
    scopes_.pop_back();
    current_fn_ = nullptr;
    if (raw->ret != Ty::Void && !returns(raw->body.get()))
      fail(name.line, name.col, quote(raw->name) + " does not return a value on every path");
    if (is_main) main_ = raw;
  }

  // Does every path through s end in a return?
  static bool returns(const Stmt* s) {
    switch (s->k) {
      case Stmt::RETURN: return true;
      case Stmt::BLOCK:
        for (const auto& c : s->body)
          if (returns(c.get())) return true;
        return false;
      case Stmt::IF: return s->else_s && returns(s->then_s.get()) && returns(s->else_s.get());
      default: return false;
    }
  }

  // --- statements ---
  std::unique_ptr<Stmt> make_stmt(Stmt::Kind k, const Token& at) {
    auto s = std::make_unique<Stmt>();
    s->k = k;
    s->line = at.line;
    s->col = at.col;
    return s;
  }

  std::unique_ptr<Stmt> parse_block(bool new_scope) {
    const Token& open = expect(T_LBRACE, "'{'");
    DepthGuard g(*this, open);
    auto s = make_stmt(Stmt::BLOCK, open);
    if (new_scope) scopes_.emplace_back();
    while (peek().t != T_RBRACE) {
      if (peek().t == T_END) fail(open.line, open.col, "'{' is never closed");
      s->body.push_back(parse_stmt());
    }
    next();
    if (new_scope) scopes_.pop_back();
    return s;
  }

  // A statement that is its own scope (the body of if / for).
  std::unique_ptr<Stmt> parse_sub_stmt() {
    if (peek().t == T_LBRACE) return parse_block(true);
    DepthGuard g(*this, peek());  // braceless `if (a) if (b) ...` nests too
    scopes_.emplace_back();
    std::unique_ptr<Stmt> s = parse_stmt();
    scopes_.pop_back();
    return s;
  }

  std::unique_ptr<Stmt> parse_stmt() {
    const Token& t = peek();
    switch (t.t) {
      case T_LBRACE: return parse_block(true);
      case T_SEMI: next(); return make_stmt(Stmt::EMPTY, t);
      case T_IF: return parse_if();
      case T_FOR: return parse_for();
      case T_BREAK:
      case T_CONTINUE: {
        if (loop_depth_ == 0) fail(t.line, t.col, quote(t.text) + " outside a loop");
        next();
        expect(T_SEMI, "';'");
        return make_stmt(t.t == T_BREAK ? Stmt::BREAK : Stmt::CONTINUE, t);
      }
      case T_RETURN: return parse_return();
      case T_CONST: case T_LOWP: case T_MEDIUMP: case T_HIGHP:
      case T_KW_FLOAT: case T_KW_INT: case T_KW_BOOL: case T_KW_VEC2: case T_KW_VEC3: case T_KW_VEC4:
        return parse_decl();
      case T_IDENT:
        if (peek(1).t == T_LPAREN) {
          auto s = make_stmt(Stmt::CALL, t);
          s->value = parse_expr();
          if (s->value->k != Expr::CALL) fail(t.line, t.col, "expected a function call statement");
          expect(T_SEMI, "';'");
          return s;
        }
        return parse_assign();
      default:
        fail(t.line, t.col, "expected a statement, found " + quote(t.text));
    }
  }

  std::unique_ptr<Stmt> parse_decl() {
    const Token& first = peek();
    auto s = make_stmt(Stmt::DECL, first);
    const bool is_const = accept(T_CONST);
    skip_precision();
    const Token& type_tok = next();
    if (!is_type_tok(type_tok.t)) fail(type_tok.line, type_tok.col, "expected a type, found " + quote(type_tok.text));
    const Ty ty = tok_type(type_tok.t);
    if (ty == Ty::Int && !is_const)
      fail(type_tok.line, type_tok.col, "int variables must be const (ints are compile-time values)");
    do {
      const Token& name = expect(T_IDENT, "a name");
      VarDecl* v = new_var();
      v->kind = VarDecl::LOCAL;
      v->name = name.text;
      v->type = ty;
      v->is_const = is_const;
      v->line = name.line;
      v->col = name.col;
      std::unique_ptr<Expr> init;
      if (accept(T_ASSIGN)) {
        const Token& at = peek();
        init = parse_expr();
        if (!converts_to(init->type, ty))
          fail(at.line, at.col, quote(v->name) + " is " + ty_name(ty) + ", but its value is " + ty_name(init->type));
      } else if (is_const) {
        fail(name.line, name.col, "const " + quote(v->name) + " needs a value");
      }
      declare_local(v, name);  // after the initialiser: 'float x = x;' reads the outer x
      s->decls.emplace_back(v, std::move(init));
    } while (accept(T_COMMA));
    expect(T_SEMI, "';'");
    return s;
  }

  std::unique_ptr<Expr> parse_lvalue() {
    const Token& t = expect(T_IDENT, "a variable");
    const VarDecl* v = lookup(t.text);
    if (!v) fail(t.line, t.col, quote(t.text) + " is not declared");
    switch (v->kind) {
      case VarDecl::UNIFORM: fail(t.line, t.col, "uniform " + quote(v->name) + " cannot be assigned");
      case VarDecl::RESERVED: fail(t.line, t.col, quote(v->name) + " cannot be assigned");
      case VarDecl::LOOP_COUNTER: fail(t.line, t.col, "loop counter " + quote(v->name) + " cannot be assigned");
      default: break;
    }
    if (v->is_const) fail(t.line, t.col, "const " + quote(v->name) + " cannot be assigned");
    if (v->type == Ty::Int) fail(t.line, t.col, "int " + quote(v->name) + " cannot be assigned");
    auto e = std::make_unique<Expr>();
    e->k = Expr::VAR;
    e->line = t.line;
    e->col = t.col;
    e->var = v;
    e->type = v->type;
    if (peek().t == T_DOT) {
      const Token& field = peek(1);
      e = parse_swizzle(std::move(e));
      for (int i = 0; i < e->swz_n; ++i)
        for (int j = 0; j < i; ++j)
          if (e->swz[i] == e->swz[j]) fail(field.line, field.col, "an assigned swizzle cannot repeat a component");
    }
    return e;
  }

  std::unique_ptr<Stmt> parse_assign() {
    const Token& t = peek();
    auto s = make_stmt(Stmt::ASSIGN, t);
    s->target = parse_lvalue();
    const Token& op = next();
    if (!(op.t == T_ASSIGN || op.t == T_PLUSEQ || op.t == T_MINUSEQ || op.t == T_STAREQ || op.t == T_SLASHEQ)) {
      if (op.t == T_PLUSPLUS || op.t == T_MINUSMINUS)
        fail(op.line, op.col, "'++' and '--' are only allowed in a for loop update");
      fail(op.line, op.col, "expected an assignment, found " + quote(op.text));
    }
    s->assign_op = op.t;
    const Token& at = peek();
    s->value = parse_expr();
    const Ty tt = s->target->type;
    if (op.t == T_ASSIGN) {
      if (!converts_to(s->value->type, tt))
        fail(at.line, at.col, "cannot assign " + std::string(ty_name(s->value->type)) + " to " + ty_name(tt));
    } else {
      const Tok bin = op.t == T_PLUSEQ ? T_PLUS : op.t == T_MINUSEQ ? T_MINUS : op.t == T_STAREQ ? T_STAR : T_SLASH;
      const Ty r = arith_type(bin, tt, s->value->type, op);
      if (r != tt) fail(op.line, op.col, quote(op.text) + " would make " + ty_name(tt) + " a " + ty_name(r));
    }
    expect(T_SEMI, "';'");
    return s;
  }

  std::unique_ptr<Stmt> parse_if() {
    const Token& t = next();
    auto s = make_stmt(Stmt::IF, t);
    expect(T_LPAREN, "'('");
    const Token& at = peek();
    s->cond = parse_expr();
    if (s->cond->type != Ty::Bool) fail(at.line, at.col, "the condition is " + std::string(ty_name(s->cond->type)) + ", expected bool");
    expect(T_RPAREN, "')'");
    s->then_s = parse_sub_stmt();
    if (accept(T_ELSE)) s->else_s = parse_sub_stmt();
    return s;
  }

  std::unique_ptr<Stmt> parse_for() {
    const Token& t = next();
    auto s = make_stmt(Stmt::FOR, t);
    expect(T_LPAREN, "'('");
    if (peek().t != T_KW_INT) fail(peek().line, peek().col, "a for loop starts with 'int <counter> = <start>'");
    next();
    const Token& name = expect(T_IDENT, "the loop counter");
    expect(T_ASSIGN, "'='");
    scopes_.emplace_back();
    const Token& init_at = peek();
    s->for_init = parse_expr();
    if (s->for_init->type != Ty::Int) fail(init_at.line, init_at.col, "the loop start must be an int");
    VarDecl* v = new_var();
    v->kind = VarDecl::LOOP_COUNTER;
    v->name = name.text;
    v->type = Ty::Int;
    v->line = name.line;
    v->col = name.col;
    declare_local(v, name);
    s->counter = v;
    expect(T_SEMI, "';'");
    const Token& cv = expect(T_IDENT, "the loop counter");
    if (cv.text != name.text) fail(cv.line, cv.col, "the loop condition must test " + quote(name.text));
    const Token& cmp = next();
    if (!(cmp.t == T_LT || cmp.t == T_LE || cmp.t == T_GT || cmp.t == T_GE || cmp.t == T_NE))
      fail(cmp.line, cmp.col, "the loop condition must be <, <=, >, >= or !=");
    s->for_cmp = cmp.t;
    const Token& bound_at = peek();
    s->for_bound = parse_add();
    if (s->for_bound->type != Ty::Int) fail(bound_at.line, bound_at.col, "the loop bound must be an int");
    expect(T_SEMI, "';'");
    // update: i++ / ++i / i-- / --i / i += C / i -= C
    const Token& u = peek();
    if (u.t == T_PLUSPLUS || u.t == T_MINUSMINUS) {
      next();
      const Token& uv = expect(T_IDENT, "the loop counter");
      if (uv.text != name.text) fail(uv.line, uv.col, "the loop update must change " + quote(name.text));
      s->for_dir = u.t == T_PLUSPLUS ? 1 : -1;
    } else {
      const Token& uv = expect(T_IDENT, "the loop counter");
      if (uv.text != name.text) fail(uv.line, uv.col, "the loop update must change " + quote(name.text));
      const Token& uo = next();
      if (uo.t == T_PLUSPLUS || uo.t == T_MINUSMINUS) {
        s->for_dir = uo.t == T_PLUSPLUS ? 1 : -1;
      } else if (uo.t == T_PLUSEQ || uo.t == T_MINUSEQ) {
        s->for_dir = uo.t == T_PLUSEQ ? 1 : -1;
        const Token& step_at = peek();
        s->for_step = parse_expr();
        if (s->for_step->type != Ty::Int) fail(step_at.line, step_at.col, "the loop step must be an int");
      } else {
        fail(uo.line, uo.col, "the loop update must be ++, --, += or -=");
      }
    }
    expect(T_RPAREN, "')'");
    ++loop_depth_;
    s->then_s = parse_sub_stmt();
    --loop_depth_;
    scopes_.pop_back();
    return s;
  }

  std::unique_ptr<Stmt> parse_return() {
    const Token& t = next();
    auto s = make_stmt(Stmt::RETURN, t);
    const Ty want = current_fn_->ret;
    if (peek().t == T_SEMI) {
      if (want != Ty::Void) fail(t.line, t.col, quote(current_fn_->name) + " must return " + ty_name(want));
    } else {
      const Token& at = peek();
      s->value = parse_expr();
      if (want == Ty::Void) fail(at.line, at.col, quote(current_fn_->name) + " returns void");
      if (!converts_to(s->value->type, want))
        fail(at.line, at.col, "returns " + std::string(ty_name(s->value->type)) + ", expected " + ty_name(want));
    }
    expect(T_SEMI, "';'");
    return s;
  }

  // --- expressions ---
  std::unique_ptr<Expr> make_expr(Expr::Kind k, const Token& at, Ty ty) {
    auto e = std::make_unique<Expr>();
    e->k = k;
    e->line = at.line;
    e->col = at.col;
    e->type = ty;
    return e;
  }

  // Result type of an arithmetic operator, or a failure at `at`.
  Ty arith_type(Tok op, Ty a, Ty b, const Token& at) {
    const bool numeric = a != Ty::Bool && b != Ty::Bool && a != Ty::Void && b != Ty::Void;
    if (numeric && a == Ty::Int && b == Ty::Int) {
      if (op == T_SLASH) fail(at.line, at.col, "int division; write 1.0 / 2.0");
      return Ty::Int;
    }
    const Ty fa = a == Ty::Int ? Ty::Float : a, fb = b == Ty::Int ? Ty::Float : b;
    if (numeric && fa == fb) return fa;
    if (numeric && fa == Ty::Float) return fb;
    if (numeric && fb == Ty::Float) return fa;
    fail(at.line, at.col, "cannot apply " + quote(at.text) + " to " + ty_name(a) + " and " + ty_name(b));
  }

  std::unique_ptr<Expr> binary(Tok op, const Token& at, std::unique_ptr<Expr> l, std::unique_ptr<Expr> r, Ty ty) {
    auto e = make_expr(Expr::BINARY, at, ty);
    e->op = op;
    e->args.push_back(std::move(l));
    e->args.push_back(std::move(r));
    return e;
  }

  std::unique_ptr<Expr> parse_expr() {
    DepthGuard g(*this, peek());
    std::unique_ptr<Expr> c = parse_or();
    if (peek().t != T_QUESTION) return c;
    const Token& q = next();
    if (c->type != Ty::Bool) fail(q.line, q.col, "the condition of '?:' must be bool");
    std::unique_ptr<Expr> a = parse_expr();
    expect(T_COLON, "':'");
    std::unique_ptr<Expr> b = parse_expr();
    Ty ty;
    if (a->type == b->type) ty = a->type;
    else if (converts_to(a->type, b->type)) ty = b->type;
    else if (converts_to(b->type, a->type)) ty = a->type;
    else fail(q.line, q.col, "the two sides of '?:' are " + std::string(ty_name(a->type)) + " and " + ty_name(b->type));
    if (ty == Ty::Void) fail(q.line, q.col, "'?:' cannot select void");
    auto e = make_expr(Expr::TERNARY, q, ty);
    e->args.push_back(std::move(c));
    e->args.push_back(std::move(a));
    e->args.push_back(std::move(b));
    return e;
  }

  std::unique_ptr<Expr> parse_logic(int level) {
    static const Tok ops[3] = {T_OROR, T_XORXOR, T_ANDAND};
    std::unique_ptr<Expr> l = level < 2 ? parse_logic(level + 1) : parse_eq();
    while (peek().t == ops[level]) {
      const Token& op = next();
      std::unique_ptr<Expr> r = level < 2 ? parse_logic(level + 1) : parse_eq();
      if (l->type != Ty::Bool || r->type != Ty::Bool)
        fail(op.line, op.col, quote(op.text) + " needs bool operands");
      l = binary(op.t, op, std::move(l), std::move(r), Ty::Bool);
    }
    return l;
  }
  std::unique_ptr<Expr> parse_or() { return parse_logic(0); }

  std::unique_ptr<Expr> parse_eq() {
    std::unique_ptr<Expr> l = parse_rel();
    while (peek().t == T_EQEQ || peek().t == T_NE) {
      const Token& op = next();
      std::unique_ptr<Expr> r = parse_rel();
      if (!(converts_to(l->type, r->type) || converts_to(r->type, l->type)) || l->type == Ty::Void)
        fail(op.line, op.col, "cannot compare " + std::string(ty_name(l->type)) + " with " + ty_name(r->type));
      l = binary(op.t, op, std::move(l), std::move(r), Ty::Bool);
    }
    return l;
  }

  std::unique_ptr<Expr> parse_rel() {
    std::unique_ptr<Expr> l = parse_add();
    const Tok t = peek().t;
    if (t == T_LT || t == T_GT || t == T_LE || t == T_GE) {
      const Token& op = next();
      std::unique_ptr<Expr> r = parse_add();
      auto scalar = [](Ty x) { return x == Ty::Float || x == Ty::Int; };
      if (!scalar(l->type) || !scalar(r->type))
        fail(op.line, op.col, quote(op.text) + " compares scalars only, not " + ty_name(l->type) + " and " + ty_name(r->type));
      l = binary(op.t, op, std::move(l), std::move(r), Ty::Bool);
    }
    return l;
  }

  std::unique_ptr<Expr> parse_add() {
    std::unique_ptr<Expr> l = parse_mul();
    while (peek().t == T_PLUS || peek().t == T_MINUS) {
      const Token& op = next();
      std::unique_ptr<Expr> r = parse_mul();
      const Ty ty = arith_type(op.t, l->type, r->type, op);
      l = binary(op.t, op, std::move(l), std::move(r), ty);
    }
    return l;
  }

  std::unique_ptr<Expr> parse_mul() {
    std::unique_ptr<Expr> l = parse_unary();
    while (peek().t == T_STAR || peek().t == T_SLASH) {
      const Token& op = next();
      std::unique_ptr<Expr> r = parse_unary();
      const Ty ty = arith_type(op.t, l->type, r->type, op);
      l = binary(op.t, op, std::move(l), std::move(r), ty);
    }
    return l;
  }

  std::unique_ptr<Expr> parse_unary() {
    const Token& t = peek();
    if (t.t == T_MINUS || t.t == T_PLUS || t.t == T_BANG) {
      DepthGuard g(*this, t);
      next();
      std::unique_ptr<Expr> a = parse_unary();
      if (t.t == T_BANG) {
        if (a->type != Ty::Bool) fail(t.line, t.col, "'!' needs a bool");
      } else if (!(ty_floaty(a->type) || a->type == Ty::Int)) {
        fail(t.line, t.col, "cannot apply " + quote(t.text) + " to " + ty_name(a->type));
      }
      if (t.t == T_PLUS) return a;
      auto e = make_expr(Expr::UNARY, t, a->type);
      e->op = t.t;
      e->args.push_back(std::move(a));
      return e;
    }
    if (t.t == T_PLUSPLUS || t.t == T_MINUSMINUS)
      fail(t.line, t.col, "'++' and '--' are only allowed in a for loop update");
    return parse_postfix();
  }

  std::unique_ptr<Expr> parse_swizzle(std::unique_ptr<Expr> base) {
    const Token& dot = next();
    const Token& f = expect(T_IDENT, "a swizzle");
    if (!ty_vec(base->type)) fail(dot.line, dot.col, "cannot swizzle " + std::string(ty_name(base->type)));
    static const char* const sets[3] = {"xyzw", "rgba", "stpq"};
    const std::string& s = f.text;
    if (s.empty() || s.size() > 4) fail(f.line, f.col, "a swizzle has 1 to 4 components");
    int set = -1;
    for (int k = 0; k < 3; ++k)
      if (std::string(sets[k]).find(s[0]) != std::string::npos) set = k;
    if (set < 0) fail(f.line, f.col, quote(s) + " is not a swizzle");
    auto e = make_expr(Expr::SWIZZLE, f, vec_of(static_cast<int>(s.size())));
    e->line = base->line;  // the expression starts where its base does
    e->col = base->col;
    for (size_t i = 0; i < s.size(); ++i) {
      const size_t idx = std::string(sets[set]).find(s[i]);
      if (idx == std::string::npos) fail(f.line, f.col, "swizzle " + quote(s) + " mixes component sets");
      if (static_cast<int>(idx) >= ty_comps(base->type))
        fail(f.line, f.col, std::string(ty_name(base->type)) + " has no component " + quote(std::string(1, s[i])));
      e->swz[i] = static_cast<uint8_t>(idx);
    }
    e->swz_n = static_cast<int>(s.size());
    e->args.push_back(std::move(base));
    return e;
  }

  std::unique_ptr<Expr> parse_postfix() {
    std::unique_ptr<Expr> e = parse_primary();
    while (peek().t == T_DOT) e = parse_swizzle(std::move(e));
    return e;
  }

  std::vector<std::unique_ptr<Expr>> parse_args() {
    std::vector<std::unique_ptr<Expr>> args;
    expect(T_LPAREN, "'('");
    if (peek().t != T_RPAREN) {
      do args.push_back(parse_expr());
      while (accept(T_COMMA));
    }
    expect(T_RPAREN, "')'");
    return args;
  }

  std::unique_ptr<Expr> parse_primary() {
    const Token& t = peek();
    DepthGuard g(*this, t);
    switch (t.t) {
      case T_INT: { next(); auto e = make_expr(Expr::INT, t, Ty::Int); e->ival = t.ival; return e; }
      case T_FLOAT: { next(); auto e = make_expr(Expr::FLOAT, t, Ty::Float); e->fval = t.fval; return e; }
      case T_TRUE: case T_FALSE: {
        next();
        auto e = make_expr(Expr::BOOL, t, Ty::Bool);
        e->ival = t.t == T_TRUE ? 1 : 0;
        return e;
      }
      case T_LPAREN: {
        next();
        std::unique_ptr<Expr> e = parse_expr();
        expect(T_RPAREN, "')'");
        return e;
      }
      case T_KW_FLOAT: case T_KW_VEC2: case T_KW_VEC3: case T_KW_VEC4: return parse_ctor();
      case T_KW_INT: case T_KW_BOOL:
        fail(t.line, t.col, quote(t.text) + "(...) does not exist; only float(...) and vecN(...) construct values");
      case T_IDENT: {
        if (peek(1).t == T_LPAREN) return parse_call();
        next();
        const VarDecl* v = lookup(t.text);
        if (!v) fail(t.line, t.col, quote(t.text) + " is not declared");
        auto e = make_expr(Expr::VAR, t, v->type);
        e->var = v;
        return e;
      }
      default:
        fail(t.line, t.col, "expected an expression, found " + quote(t.text));
    }
  }

  std::unique_ptr<Expr> parse_ctor() {
    const Token& t = next();
    const Ty ty = tok_type(t.t);
    auto e = make_expr(Expr::CTOR, t, ty);
    e->args = parse_args();
    const int want = ty_comps(ty);
    if (e->args.empty()) fail(t.line, t.col, quote(t.text) + "() needs arguments");
    if (ty == Ty::Float) {
      if (e->args.size() != 1 || !(e->args[0]->type == Ty::Float || e->args[0]->type == Ty::Int || e->args[0]->type == Ty::Bool))
        fail(t.line, t.col, "float(...) takes one float, int or bool");
      return e;
    }
    if (e->args.size() == 1) {
      const Ty a = e->args[0]->type;
      if (a == Ty::Float || a == Ty::Int) return e;              // splat
      if (ty_vec(a) && ty_comps(a) >= want) return e;            // keep the first N
      fail(t.line, t.col, std::string("cannot construct ") + ty_name(ty) + " from " + ty_name(a));
    }
    int total = 0;
    for (const auto& a : e->args) {
      if (!(ty_floaty(a->type) || a->type == Ty::Int))
        fail(a->line, a->col, std::string("cannot use ") + ty_name(a->type) + " to construct " + ty_name(ty));
      total += ty_comps(a->type);
    }
    if (total != want)
      fail(t.line, t.col, std::string(ty_name(ty)) + "(...) needs " + std::to_string(want) + " components, got " + std::to_string(total));
    return e;
  }

  // Common type of component-wise builtin arguments: scalars broadcast.
  Ty common_type(const Expr& call, const Token& t) {
    Ty ty = Ty::Float;
    for (const auto& a : call.args) {
      if (!(ty_floaty(a->type) || a->type == Ty::Int))
        fail(a->line, a->col, quote(t.text) + " takes float or vector arguments, not " + ty_name(a->type));
      if (ty_vec(a->type)) {
        if (ty_vec(ty) && ty != a->type)
          fail(a->line, a->col, quote(t.text) + " mixes " + ty_name(ty) + " and " + ty_name(a->type));
        ty = a->type;
      }
    }
    return ty;
  }

  std::unique_ptr<Expr> parse_call() {
    const Token& t = next();
    if (t.text == "main") fail(t.line, t.col, "main cannot be called");
    const Builtin b = find_builtin(t.text);
    if (b != B_NONE) {
      auto e = make_expr(Expr::CALL, t, Ty::Float);
      e->builtin = b;
      e->args = parse_args();
      const int n = static_cast<int>(e->args.size());
      auto want_n = [&](int k) {
        if (n != k) fail(t.line, t.col, quote(t.text) + " takes " + std::to_string(k) + " argument" + (k == 1 ? "" : "s"));
      };
      if (b <= B_SATURATE) want_n(1);
      else if (b == B_ATAN) { if (n != 1 && n != 2) fail(t.line, t.col, "'atan' takes 1 or 2 arguments"); }
      else if (b <= B_POW) want_n(2);
      else if (b <= B_SMOOTHSTEP) want_n(3);
      else if (b == B_LENGTH || b == B_NORMALIZE || b == B_HASH || b == B_NOISE) want_n(1);
      else want_n(2);
      const Ty ct = common_type(*e, t);
      switch (b) {
        case B_LENGTH: case B_DISTANCE: case B_DOT:
          if (n == 2 && e->args[0]->type != e->args[1]->type && !(ty_comps(e->args[0]->type) == 1 && ty_comps(e->args[1]->type) == 1))
            fail(t.line, t.col, quote(t.text) + " needs two arguments of the same type");
          e->type = Ty::Float;
          break;
        case B_NORMALIZE: e->type = ct; break;
        case B_REFLECT:
          if (e->args[0]->type != e->args[1]->type && !(ty_comps(e->args[0]->type) == 1 && ty_comps(e->args[1]->type) == 1))
            fail(t.line, t.col, "'reflect' needs two arguments of the same type");
          e->type = ct;
          break;
        case B_CROSS:
          if (e->args[0]->type != Ty::Vec3 || e->args[1]->type != Ty::Vec3) fail(t.line, t.col, "'cross' takes two vec3");
          e->type = Ty::Vec3;
          break;
        case B_HASH: case B_NOISE:
          if (e->args[0]->type != Ty::Vec2) fail(t.line, t.col, quote(t.text) + " takes a vec2");
          e->type = Ty::Float;
          break;
        default: e->type = ct; break;
      }
      return e;
    }
    if (current_fn_ && t.text == current_fn_->name)
      fail(t.line, t.col, quote(t.text) + " calls itself; recursion is not supported");
    auto it = functions_.find(t.text);
    if (it == functions_.end()) {
      if (lookup(t.text)) fail(t.line, t.col, quote(t.text) + " is not a function");
      fail(t.line, t.col, quote(t.text) + " is not declared (functions must be defined before they are called)");
    }
    const Function* fn = it->second.get();
    auto e = make_expr(Expr::CALL, t, fn->ret);
    e->fn = fn;
    e->args = parse_args();
    if (e->args.size() != fn->params.size())
      fail(t.line, t.col, quote(fn->name) + " takes " + std::to_string(fn->params.size()) + " arguments");
    for (size_t i = 0; i < e->args.size(); ++i) {
      const VarDecl* p = fn->params[i];
      const Expr& a = *e->args[i];
      if (p->qual == Q_IN) {
        if (!converts_to(a.type, p->type))
          fail(a.line, a.col, "argument " + std::to_string(i + 1) + " of " + quote(fn->name) + " is " + ty_name(a.type) + ", expected " + ty_name(p->type));
      } else {
        const Expr* base = a.k == Expr::SWIZZLE ? a.args[0].get() : &a;
        const bool lvalue = base->k == Expr::VAR && base->var->kind != VarDecl::UNIFORM &&
                            base->var->kind != VarDecl::RESERVED && base->var->kind != VarDecl::LOOP_COUNTER &&
                            !base->var->is_const && base->var->type != Ty::Int;
        if (!lvalue) fail(a.line, a.col, "argument " + std::to_string(i + 1) + " of " + quote(fn->name) + " is 'out' and needs a variable");
        if (a.k == Expr::SWIZZLE)
          for (int x = 0; x < a.swz_n; ++x)
            for (int y = 0; y < x; ++y)
              if (a.swz[x] == a.swz[y]) fail(a.line, a.col, "an 'out' swizzle cannot repeat a component");
        if (a.type != p->type)
          fail(a.line, a.col, "argument " + std::to_string(i + 1) + " of " + quote(fn->name) + " is " + ty_name(a.type) + ", expected " + ty_name(p->type));
      }
    }
    return e;
  }

  std::vector<Token> toks_;
  size_t pos_ = 0;
  uint32_t flags_;
  int depth_ = 0;
  int loop_depth_ = 0;
  std::vector<std::unique_ptr<VarDecl>> vars_;
  std::vector<std::unique_ptr<Expr>> owned_exprs_;
  std::unordered_map<std::string, const VarDecl*> globals_;
  std::unordered_map<std::string, std::unique_ptr<Function>> functions_;
  std::vector<std::unordered_map<std::string, const VarDecl*>> scopes_;
  std::vector<const VarDecl*> uniform_list_;
  std::vector<const VarDecl*> const_list_;
  int uniform_values_ = 0;
  const Function* current_fn_ = nullptr;
  const Function* main_ = nullptr;
};

// ---------------------------------------------------------------------------
// IR builder: SSA values with constant folding, exact simplifications and
// common-subexpression sharing.
// ---------------------------------------------------------------------------

class IR {
 public:
  enum Kind : uint8_t { K_CONST, K_UNIFORM, K_RESERVED, K_INPUT, K_INS };
  struct Val {
    Kind kind = K_CONST;
    Op op = OP_ADD;
    int a = -1, b = -1, c = -1;
    float value = 0.0f;
    int slot = 0;
    bool varying = false;
  };

  std::vector<Val> v;

  int k(float x) {
    const uint32_t key = bits(x);
    auto it = consts_.find(key);
    if (it != consts_.end()) return it->second;
    Val val;
    val.kind = K_CONST;
    val.value = x;
    return consts_[key] = push(val);
  }
  int t() { return k(1.0f); }
  int f() { return k(0.0f); }

  int leaf(Kind kind, int slot) {
    const uint64_t key = (static_cast<uint64_t>(kind) << 32) | static_cast<uint32_t>(slot);
    auto it = leaves_.find(key);
    if (it != leaves_.end()) return it->second;
    Val val;
    val.kind = kind;
    val.slot = slot;
    val.varying = kind == K_INPUT;
    const int id = push(val);
    leaves_[key] = id;
    if (kind == K_INPUT) input_ids[slot] = id;
    return id;
  }

  int input_ids[2] = {-1, -1};  // the pixel position, once lowering created it

  bool is_const(int id) const { return v[static_cast<size_t>(id)].kind == K_CONST; }
  float cval(int id) const { return v[static_cast<size_t>(id)].value; }
  bool is_true(int id) const { return is_const(id) && cval(id) != 0.0f; }
  bool is_false(int id) const { return is_const(id) && cval(id) == 0.0f; }

  int op(Op o, int a, int b = -1, int c = -1) {
    const int ar = op_arity(o);
    if (ar < 2) b = -1;
    if (ar < 3) c = -1;
    if (is_const(a) && (b < 0 || is_const(b)) && (c < 0 || is_const(c)))
      return k(eval_op(o, cval(a), b < 0 ? 0.0f : cval(b), c < 0 ? 0.0f : cval(c)));
    // Exact rewrites only: each returns the very bits the op would compute
    // (NaN payloads are not observable).
    switch (o) {
      case OP_MUL:
        if (is_const(b) && bits(cval(b)) == 0x3f800000u) return a;
        if (is_const(a) && bits(cval(a)) == 0x3f800000u) return b;
        break;
      case OP_DIV:
        if (is_const(b) && bits(cval(b)) == 0x3f800000u) return a;
        break;
      case OP_SUB:
        if (is_const(b) && bits(cval(b)) == 0u) return a;  // x - (+0) == x
        break;
      case OP_ADD:
        if (is_const(b) && bits(cval(b)) == 0x80000000u) return a;  // x + (-0) == x
        if (is_const(a) && bits(cval(a)) == 0x80000000u) return b;
        break;
      case OP_NEG: {
        const Val& x = v[static_cast<size_t>(a)];
        if (x.kind == K_INS && x.op == OP_NEG) return x.a;
        break;
      }
      case OP_SEL:
        if (is_const(a)) return cval(a) != 0.0f ? b : c;
        if (b == c) return b;
        break;
      case OP_MIN: case OP_MAX:
        if (a == b) return a;
        break;
      default:
        break;
    }
    const uint64_t key = (static_cast<uint64_t>(o) << 54) | (static_cast<uint64_t>(a + 1) << 36) |
                         (static_cast<uint64_t>(b + 1) << 18) | static_cast<uint64_t>(c + 1);
    auto it = cse_.find(key);
    if (it != cse_.end()) return it->second;
    Val val;
    val.kind = K_INS;
    val.op = o;
    val.a = a;
    val.b = b;
    val.c = c;
    val.varying = v[static_cast<size_t>(a)].varying || (b >= 0 && v[static_cast<size_t>(b)].varying) ||
                  (c >= 0 && v[static_cast<size_t>(c)].varying);
    return cse_[key] = push(val);
  }

  // Booleans (0 / 1).
  int and_(int a, int b) {
    if (is_false(a) || is_false(b)) return f();
    return op(OP_MUL, a, b);
  }
  int or_(int a, int b) {
    if (is_true(a) || is_true(b)) return t();
    if (is_false(a)) return b;
    if (is_false(b)) return a;
    return op(OP_MAX, a, b);
  }
  int not_(int a) { return op(OP_SUB, t(), a); }

 private:
  int push(const Val& val) {
    if (v.size() >= k_max_ir_values) fail(0, 0, "");  // replaced by the caller's location
    v.push_back(val);
    return static_cast<int>(v.size() - 1);
  }

  std::unordered_map<uint32_t, int> consts_;
  std::unordered_map<uint64_t, int> leaves_;
  std::unordered_map<uint64_t, int> cse_;
};

// ---------------------------------------------------------------------------
// Lowering
// ---------------------------------------------------------------------------

struct RV {
  Ty t = Ty::Void;
  int n = 0;
  int id[4] = {-1, -1, -1, -1};
  int64_t ival = 0;
};

class Lowerer {
 public:
  Lowerer(IR& ir, const Parser& ps) : ir_(ir), ps_(ps) {}

  // Uniform values and global consts; before uniform defaults and main.
  void lower_globals() {
    for (const VarDecl* u : ps_.uniforms()) {
      RV r;
      r.t = u->type;
      r.n = ty_comps(u->type);
      for (int i = 0; i < r.n; ++i) r.id[i] = ir_.leaf(IR::K_UNIFORM, u->slot + i);
      globals_[u] = r;
    }
    frames_.emplace_back();  // a frame for constant initialisers
    frames_.back().path = frames_.back().ret_alive = ir_.t();
    for (const VarDecl* c : ps_.global_consts()) {
      RV r = convert(lower_const_expr(c->init, c->name), c->type);
      globals_[c] = r;
    }
    frames_.pop_back();
  }

  void lower_main(int out[4]) {
    const Function* m = ps_.main_fn();
    RV p;
    p.t = Ty::Vec2;
    p.n = 2;
    p.id[0] = ir_.leaf(IR::K_INPUT, 0);
    p.id[1] = ir_.leaf(IR::K_INPUT, 1);
    std::vector<RV> args{p};
    const RV r = inline_call(m, args, m->line, m->col);
    for (int i = 0; i < 4; ++i) out[i] = r.id[i];
  }

  // Constant value of a uniform default (all components folded).
  RV uniform_default(const VarDecl* u) {
    frames_.emplace_back();
    frames_.back().path = frames_.back().ret_alive = ir_.t();
    RV r = convert(lower_const_expr(u->init, u->name), u->type);
    frames_.pop_back();
    return r;
  }

 private:
  struct VarState {
    RV val;
    int depth = 0;       // loop depth of the declaration within its function
    bool out_param = false;
  };
  struct Loop {
    int brk = -1;   // pixels that have not broken out
    int iter = -1;  // pixels still running this iteration
  };
  struct Frame {
    int path = -1;       // conjunction of the enclosing if conditions
    int ret_alive = -1;  // pixels that have not returned
    RV retval;
    std::vector<Loop> loops;
  };

  // --- helpers ---
  Frame& frame() { return frames_.back(); }
  int cum_alive() { return frame().loops.empty() ? ir_.t() : frame().loops.back().iter; }

  RV scalar(int id, Ty t = Ty::Float) {
    RV r;
    r.t = t;
    r.n = 1;
    r.id[0] = id;
    return r;
  }
  RV int_val(int64_t i) {
    RV r;
    r.t = Ty::Int;
    r.n = 1;
    r.ival = i;
    return r;
  }

  // Int -> float, and a scalar broadcast to n components.
  RV floats(const RV& x, int n = 0) {
    RV r = x;
    if (x.t == Ty::Int) {
      r.t = Ty::Float;
      r.n = 1;
      r.id[0] = ir_.k(static_cast<float>(x.ival));
    }
    if (n > 1 && r.n == 1) {
      for (int i = 1; i < n; ++i) r.id[i] = r.id[0];
      r.n = n;
      r.t = vec_of(n);
    }
    return r;
  }
  RV convert(const RV& x, Ty to) {
    if (x.t == Ty::Int && to == Ty::Float) return floats(x);
    return x;
  }

  [[noreturn]] void error_at(const Expr* e, const std::string& msg) { fail(e->line, e->col, msg); }

  int64_t check_int(int64_t v, const Expr* e) {
    if (v > 2147483647 || v < -2147483647 - 1) error_at(e, "int overflow");
    return v;
  }

  RV lower_const_expr(const Expr* e, const std::string& name) {
    ++const_mode_;
    RV r = lower_expr(e);
    --const_mode_;
    if (r.t != Ty::Int)
      for (int i = 0; i < r.n; ++i)
        if (!ir_.is_const(r.id[i])) error_at(e, quote(name) + " needs a constant value");
    return r;
  }

  int64_t const_int(const Expr* e, const char* what) {
    const RV r = lower_expr(e);
    if (r.t != Ty::Int) error_at(e, std::string(what) + " must be an int");
    return r.ival;
  }

  // --- expressions ---
  RV lower_expr(const Expr* e) {
    Nest nest(*this, e->line, e->col);
    switch (e->k) {
      case Expr::INT: return int_val(e->ival);
      case Expr::FLOAT: return scalar(ir_.k(e->fval));
      case Expr::BOOL: return scalar(ir_.k(e->ival ? 1.0f : 0.0f), Ty::Bool);
      case Expr::VAR: return read_var(e);
      case Expr::SWIZZLE: {
        const RV base = lower_expr(e->args[0].get());
        RV r;
        r.t = e->type;
        r.n = e->swz_n;
        for (int i = 0; i < e->swz_n; ++i) r.id[i] = base.id[e->swz[i]];
        return r;
      }
      case Expr::UNARY: {
        const RV a = lower_expr(e->args[0].get());
        if (e->op == T_BANG) return scalar(ir_.not_(a.id[0]), Ty::Bool);
        if (a.t == Ty::Int) return int_val(check_int(-a.ival, e));
        RV r = a;
        for (int i = 0; i < a.n; ++i) r.id[i] = ir_.op(OP_NEG, a.id[i]);
        return r;
      }
      case Expr::BINARY: return lower_binary(e);
      case Expr::TERNARY: {
        const RV c = lower_expr(e->args[0].get());
        if (ir_.is_const(c.id[0])) {
          const RV x = lower_expr(e->args[ir_.is_true(c.id[0]) ? 1 : 2].get());
          return convert(x, e->type);
        }
        if (e->type == Ty::Int) error_at(e, "an int value cannot depend on a runtime condition");
        const RV a = floats(lower_expr(e->args[1].get()));
        const RV b = floats(lower_expr(e->args[2].get()));
        RV r = a;
        r.t = e->type;
        for (int i = 0; i < a.n; ++i) r.id[i] = ir_.op(OP_SEL, c.id[0], a.id[i], b.id[i]);
        return r;
      }
      case Expr::CTOR: return lower_ctor(e);
      case Expr::CALL:
        if (e->builtin != B_NONE) return lower_builtin(e);
        if (const_mode_) error_at(e, "a constant value cannot call " + quote(e->fn->name));
        return lower_user_call(e);
    }
    return RV{};
  }

  RV read_var(const Expr* e) {
    const VarDecl* v = e->var;
    switch (v->kind) {
      case VarDecl::UNIFORM:
        if (const_mode_) error_at(e, "uniform " + quote(v->name) + " is not a constant");
        return globals_.at(v);
      case VarDecl::GLOBAL_CONST: {
        auto it = globals_.find(v);
        if (it == globals_.end()) error_at(e, quote(v->name) + " is used before its value is known");
        return it->second;
      }
      case VarDecl::RESERVED: {
        if (const_mode_) error_at(e, quote(v->name) + " is not a constant");
        if (v->slot == RES_SIZE_X) {
          RV r;
          r.t = Ty::Vec2;
          r.n = 2;
          r.id[0] = ir_.leaf(IR::K_RESERVED, RES_SIZE_X);
          r.id[1] = ir_.leaf(IR::K_RESERVED, RES_SIZE_Y);
          return r;
        }
        return scalar(ir_.leaf(IR::K_RESERVED, v->slot));
      }
      default: {
        auto it = env_.find(v);
        if (it == env_.end()) error_at(e, quote(v->name) + " has no value here");
        return it->second.val;
      }
    }
  }

  RV lower_binary(const Expr* e) {
    const Tok op = e->op;
    const RV a = lower_expr(e->args[0].get());
    const RV b = lower_expr(e->args[1].get());
    if (op == T_ANDAND) return scalar(ir_.and_(a.id[0], b.id[0]), Ty::Bool);
    if (op == T_OROR) return scalar(ir_.or_(a.id[0], b.id[0]), Ty::Bool);
    if (op == T_XORXOR) return scalar(ir_.op(OP_NE, a.id[0], b.id[0]), Ty::Bool);
    if (a.t == Ty::Int && b.t == Ty::Int) {
      switch (op) {
        case T_PLUS: return int_val(check_int(a.ival + b.ival, e));
        case T_MINUS: return int_val(check_int(a.ival - b.ival, e));
        case T_STAR: return int_val(check_int(a.ival * b.ival, e));
        case T_LT: return scalar(ir_.k(a.ival < b.ival ? 1.0f : 0.0f), Ty::Bool);
        case T_GT: return scalar(ir_.k(a.ival > b.ival ? 1.0f : 0.0f), Ty::Bool);
        case T_LE: return scalar(ir_.k(a.ival <= b.ival ? 1.0f : 0.0f), Ty::Bool);
        case T_GE: return scalar(ir_.k(a.ival >= b.ival ? 1.0f : 0.0f), Ty::Bool);
        case T_EQEQ: return scalar(ir_.k(a.ival == b.ival ? 1.0f : 0.0f), Ty::Bool);
        case T_NE: return scalar(ir_.k(a.ival != b.ival ? 1.0f : 0.0f), Ty::Bool);
        default: error_at(e, "unsupported int operation");
      }
    }
    if (op == T_EQEQ || op == T_NE) {
      const RV x = floats(a), y = floats(b);
      int all = ir_.t();
      for (int i = 0; i < x.n; ++i) all = ir_.and_(all, ir_.op(OP_EQ, x.id[i], y.id[i]));
      return scalar(op == T_EQEQ ? all : ir_.not_(all), Ty::Bool);
    }
    if (op == T_LT || op == T_GT || op == T_LE || op == T_GE) {
      const RV x = floats(a), y = floats(b);
      int r;
      switch (op) {
        case T_LT: r = ir_.op(OP_LT, x.id[0], y.id[0]); break;
        case T_GT: r = ir_.op(OP_LT, y.id[0], x.id[0]); break;
        case T_LE: r = ir_.op(OP_LE, x.id[0], y.id[0]); break;
        default:   r = ir_.op(OP_LE, y.id[0], x.id[0]); break;
      }
      return scalar(r, Ty::Bool);
    }
    const int n = ty_comps(e->type);
    const RV x = floats(a, n), y = floats(b, n);
    const Op o = op == T_PLUS ? OP_ADD : op == T_MINUS ? OP_SUB : op == T_STAR ? OP_MUL : OP_DIV;
    RV r;
    r.t = e->type;
    r.n = n;
    for (int i = 0; i < n; ++i) r.id[i] = ir_.op(o, x.id[i], y.id[i]);
    return r;
  }

  RV lower_ctor(const Expr* e) {
    RV r;
    r.t = e->type;
    r.n = ty_comps(e->type);
    if (e->args.size() == 1) {
      const RV a = floats(lower_expr(e->args[0].get()));
      for (int i = 0; i < r.n; ++i) r.id[i] = a.id[a.n == 1 ? 0 : i];
      return r;
    }
    int k = 0;
    for (const auto& arg : e->args) {
      const RV a = floats(lower_expr(arg.get()));
      for (int i = 0; i < a.n; ++i) r.id[k++] = a.id[i];
    }
    return r;
  }

  // --- built-ins (docs/shader-language.md section 9) ---
  int dot(const RV& a, const RV& b) {
    int s = ir_.op(OP_MUL, a.id[0], b.id[0]);
    for (int i = 1; i < a.n; ++i) s = ir_.op(OP_ADD, s, ir_.op(OP_MUL, a.id[i], b.id[i]));
    return s;
  }
  int step(int edge, int x) { return ir_.op(OP_SEL, ir_.op(OP_LT, x, edge), ir_.f(), ir_.t()); }
  int clamp(int x, int lo, int hi) { return ir_.op(OP_MIN, ir_.op(OP_MAX, x, lo), hi); }

  RV lower_builtin(const Expr* e) {
    const Builtin b = e->builtin;
    std::vector<RV> args;
    for (const auto& a : e->args) args.push_back(lower_expr(a.get()));
    const int n = (b == B_LENGTH || b == B_DISTANCE || b == B_DOT || b == B_HASH || b == B_NOISE)
                      ? 0 : ty_comps(e->type);
    for (RV& a : args) a = floats(a, n);
    RV r;
    r.t = e->type;
    r.n = ty_comps(e->type);
    auto& I = ir_;
    auto per = [&](auto fn) {
      for (int i = 0; i < r.n; ++i) r.id[i] = fn(i);
    };
    auto x = [&](int i) { return args[0].id[i]; };
    auto y = [&](int i) { return args[1].id[i]; };
    auto z = [&](int i) { return args[2].id[i]; };
    switch (b) {
      case B_ABS: per([&](int i) { return I.op(OP_ABS, x(i)); }); break;
      case B_SIGN:
        per([&](int i) {
          return I.op(OP_SEL, I.op(OP_LT, I.f(), x(i)), I.t(),
                      I.op(OP_SEL, I.op(OP_LT, x(i), I.f()), I.k(-1.0f), I.f()));
        });
        break;
      case B_FLOOR: per([&](int i) { return I.op(OP_FLOOR, x(i)); }); break;
      case B_CEIL: per([&](int i) { return I.op(OP_NEG, I.op(OP_FLOOR, I.op(OP_NEG, x(i)))); }); break;
      case B_FRACT: per([&](int i) { return I.op(OP_SUB, x(i), I.op(OP_FLOOR, x(i))); }); break;
      case B_SQRT: per([&](int i) { return I.op(OP_SQRT, x(i)); }); break;
      case B_INVSQRT: per([&](int i) { return I.op(OP_DIV, I.t(), I.op(OP_SQRT, x(i))); }); break;
      case B_SIN: per([&](int i) { return I.op(OP_SIN, x(i)); }); break;
      case B_COS: per([&](int i) { return I.op(OP_COS, x(i)); }); break;
      case B_TAN: per([&](int i) { return I.op(OP_DIV, I.op(OP_SIN, x(i)), I.op(OP_COS, x(i))); }); break;
      case B_ASIN:
        per([&](int i) {
          return I.op(OP_ATAN2, x(i), I.op(OP_SQRT, I.op(OP_SUB, I.t(), I.op(OP_MUL, x(i), x(i)))));
        });
        break;
      case B_ACOS:
        per([&](int i) {
          return I.op(OP_ATAN2, I.op(OP_SQRT, I.op(OP_SUB, I.t(), I.op(OP_MUL, x(i), x(i)))), x(i));
        });
        break;
      case B_EXP: per([&](int i) { return I.op(OP_EXP2, I.op(OP_MUL, x(i), I.k(1.44269504f))); }); break;
      case B_LOG: per([&](int i) { return I.op(OP_MUL, I.op(OP_LOG2, x(i)), I.k(0.693147182f)); }); break;
      case B_EXP2: per([&](int i) { return I.op(OP_EXP2, x(i)); }); break;
      case B_LOG2: per([&](int i) { return I.op(OP_LOG2, x(i)); }); break;
      case B_RADIANS: per([&](int i) { return I.op(OP_MUL, x(i), I.k(0.0174532924f)); }); break;
      case B_DEGREES: per([&](int i) { return I.op(OP_MUL, x(i), I.k(57.2957802f)); }); break;
      case B_SATURATE: per([&](int i) { return clamp(x(i), I.f(), I.t()); }); break;
      case B_ATAN:
        if (args.size() == 1) per([&](int i) { return I.op(OP_ATAN2, x(i), I.t()); });
        else per([&](int i) { return I.op(OP_ATAN2, x(i), y(i)); });
        break;
      case B_MOD:
        per([&](int i) { return I.op(OP_SUB, x(i), I.op(OP_MUL, y(i), I.op(OP_FLOOR, I.op(OP_DIV, x(i), y(i))))); });
        break;
      case B_MIN: per([&](int i) { return I.op(OP_MIN, x(i), y(i)); }); break;
      case B_MAX: per([&](int i) { return I.op(OP_MAX, x(i), y(i)); }); break;
      case B_STEP: per([&](int i) { return step(x(i), y(i)); }); break;
      case B_POW:
        per([&](int i) {
          const int r0 = I.op(OP_EXP2, I.op(OP_MUL, y(i), I.op(OP_LOG2, x(i))));
          const int zero_zero = I.and_(I.op(OP_EQ, x(i), I.f()), I.op(OP_EQ, y(i), I.f()));
          return I.op(OP_SEL, zero_zero, I.t(), r0);
        });
        break;
      case B_CLAMP: per([&](int i) { return clamp(x(i), y(i), z(i)); }); break;
      case B_MIX:
        per([&](int i) {
          return I.op(OP_ADD, I.op(OP_MUL, x(i), I.op(OP_SUB, I.t(), z(i))), I.op(OP_MUL, y(i), z(i)));
        });
        break;
      case B_SMOOTHSTEP:
        per([&](int i) {
          const int t0 = clamp(I.op(OP_DIV, I.op(OP_SUB, z(i), x(i)), I.op(OP_SUB, y(i), x(i))), I.f(), I.t());
          const int s = I.op(OP_MUL, I.op(OP_MUL, t0, t0), I.op(OP_SUB, I.k(3.0f), I.op(OP_MUL, I.k(2.0f), t0)));
          return I.op(OP_SEL, I.op(OP_EQ, x(i), y(i)), step(x(i), z(i)), s);
        });
        break;
      case B_LENGTH: r.id[0] = I.op(OP_SQRT, dot(args[0], args[0])); break;
      case B_DISTANCE: {
        RV d = args[0];
        for (int i = 0; i < d.n; ++i) d.id[i] = I.op(OP_SUB, args[0].id[i], args[1].id[i]);
        r.id[0] = I.op(OP_SQRT, dot(d, d));
        break;
      }
      case B_DOT: r.id[0] = dot(args[0], args[1]); break;
      case B_NORMALIZE: {
        const int d = dot(args[0], args[0]);
        const int inv = I.op(OP_DIV, I.t(), I.op(OP_SQRT, d));
        const int zero = I.op(OP_EQ, d, I.f());
        per([&](int i) { return I.op(OP_SEL, zero, I.f(), I.op(OP_MUL, x(i), inv)); });
        break;
      }
      case B_CROSS:
        r.id[0] = I.op(OP_SUB, I.op(OP_MUL, x(1), y(2)), I.op(OP_MUL, x(2), y(1)));
        r.id[1] = I.op(OP_SUB, I.op(OP_MUL, x(2), y(0)), I.op(OP_MUL, x(0), y(2)));
        r.id[2] = I.op(OP_SUB, I.op(OP_MUL, x(0), y(1)), I.op(OP_MUL, x(1), y(0)));
        break;
      case B_REFLECT: {
        const int s = I.op(OP_MUL, I.k(2.0f), dot(args[1], args[0]));
        per([&](int i) { return I.op(OP_SUB, x(i), I.op(OP_MUL, s, y(i))); });
        break;
      }
      case B_HASH: r.id[0] = I.op(OP_HASH2, x(0), x(1)); break;
      case B_NOISE: {
        const int ix = I.op(OP_FLOOR, x(0)), iy = I.op(OP_FLOOR, x(1));
        const int fx = I.op(OP_SUB, x(0), ix), fy = I.op(OP_SUB, x(1), iy);
        auto fade = [&](int f) {
          return I.op(OP_MUL, I.op(OP_MUL, f, f), I.op(OP_SUB, I.k(3.0f), I.op(OP_MUL, I.k(2.0f), f)));
        };
        const int ux = fade(fx), uy = fade(fy);
        const int ix1 = I.op(OP_ADD, ix, I.t()), iy1 = I.op(OP_ADD, iy, I.t());
        auto mix = [&](int a, int b2, int t0) {
          return I.op(OP_ADD, I.op(OP_MUL, a, I.op(OP_SUB, I.t(), t0)), I.op(OP_MUL, b2, t0));
        };
        r.id[0] = mix(mix(I.op(OP_HASH2, ix, iy), I.op(OP_HASH2, ix1, iy), ux),
                      mix(I.op(OP_HASH2, ix, iy1), I.op(OP_HASH2, ix1, iy1), ux), uy);
        break;
      }
      default:
        error_at(e, "unsupported built-in");
    }
    return r;
  }

  // --- calls ---
  RV lower_user_call(const Expr* e) {
    const Function* fn = e->fn;
    std::vector<RV> args;
    for (size_t i = 0; i < e->args.size(); ++i) {
      const VarDecl* p = fn->params[i];
      if (p->qual == Q_OUT) { args.push_back(RV{}); continue; }
      args.push_back(convert(lower_expr(e->args[i].get()), p->type));
    }
    std::vector<RV> outs;
    const RV r = inline_call(fn, args, e->line, e->col, &outs);
    for (size_t i = 0; i < e->args.size(); ++i)
      if (fn->params[i]->qual != Q_IN) assign_to(e->args[i].get(), outs[i]);
    return r;
  }

  RV inline_call(const Function* fn, const std::vector<RV>& args, int line, int col,
                 std::vector<RV>* outs = nullptr) {
    if (++call_depth_ > k_max_functions + 1) fail(line, col, "calls are nested too deeply");
    Frame fr;
    fr.path = ir_.t();
    fr.ret_alive = ir_.t();
    fr.retval.t = fn->ret;
    fr.retval.n = ty_comps(fn->ret);
    for (int i = 0; i < fr.retval.n; ++i) fr.retval.id[i] = ir_.f();
    frames_.push_back(fr);
    for (size_t i = 0; i < fn->params.size(); ++i) {
      const VarDecl* p = fn->params[i];
      VarState st;
      st.out_param = p->qual != Q_IN;
      if (p->qual == Q_OUT) {
        st.val.t = p->type;
        st.val.n = ty_comps(p->type);
        for (int k = 0; k < st.val.n; ++k) st.val.id[k] = ir_.f();
      } else {
        st.val = args[i];
      }
      env_[p] = st;
    }
    lower_stmt(fn->body.get());
    const RV r = frames_.back().retval;
    frames_.pop_back();
    if (outs) {
      outs->clear();
      for (const VarDecl* p : fn->params) outs->push_back(env_[p].val);
    }
    --call_depth_;
    return r;
  }

  // --- assignment ---
  int assign_mask(const VarState& st) {
    if (st.out_param) return ir_.and_(frame().ret_alive, cum_alive());
    if (st.depth < static_cast<int>(frame().loops.size())) return cum_alive();
    return ir_.t();
  }

  // target: VAR or SWIZZLE(VAR); value already has the target's type.
  void assign_to(const Expr* target, const RV& value) {
    const Expr* base = target->k == Expr::SWIZZLE ? target->args[0].get() : target;
    VarState& st = env_[base->var];
    const int mask = assign_mask(st);
    RV nv = st.val;
    if (target->k == Expr::SWIZZLE) {
      for (int i = 0; i < target->swz_n; ++i) nv.id[target->swz[i]] = value.id[i];
    } else {
      nv = convert(value, base->var->type);
    }
    for (int i = 0; i < nv.n; ++i) nv.id[i] = ir_.op(OP_SEL, mask, nv.id[i], st.val.id[i]);
    st.val = nv;
  }

  // --- work and nesting bounds ---
  void charge(int64_t n, int line, int col) {
    steps_ += n;
    if (steps_ > k_max_lower_steps)
      fail(line, col, "shader is too large to compile (more than " + std::to_string(k_max_lower_steps) +
                          " steps after inlining and unrolling)");
  }
  struct Nest {
    Lowerer& l;
    Nest(Lowerer& lw, int line, int col) : l(lw) {
      if (++l.depth_ > k_max_lower_depth)
        fail(line, col, "nesting is deeper than " + std::to_string(k_max_lower_depth) + " after inlining");
      l.charge(1, line, col);
    }
    ~Nest() { --l.depth_; }
    Nest(const Nest&) = delete;
    Nest& operator=(const Nest&) = delete;
  };

  // --- statements; returns true when the rest of the block is unreachable ---
  bool lower_stmt(const Stmt* s) {
    Nest nest(*this, s->line, s->col);
    switch (s->k) {
      case Stmt::EMPTY: return false;
      case Stmt::BLOCK:
        for (const auto& c : s->body)
          if (lower_stmt(c.get())) return true;
        return false;
      case Stmt::DECL:
        for (const auto& d : s->decls) {
          VarState st;
          st.depth = static_cast<int>(frame().loops.size());
          const VarDecl* v = d.first;
          if (d.second) {
            st.val = v->is_const ? lower_const_expr(d.second.get(), v->name) : lower_expr(d.second.get());
            st.val = convert(st.val, v->type);
          } else {
            st.val.t = v->type;
            st.val.n = ty_comps(v->type);
            for (int i = 0; i < st.val.n; ++i) st.val.id[i] = ir_.f();
          }
          env_[v] = st;
        }
        return false;
      case Stmt::ASSIGN: {
        RV value = lower_expr(s->value.get());
        if (s->assign_op != T_ASSIGN) {
          const RV cur = lower_expr(s->target.get());
          const Op o = s->assign_op == T_PLUSEQ ? OP_ADD : s->assign_op == T_MINUSEQ ? OP_SUB
                     : s->assign_op == T_STAREQ ? OP_MUL : OP_DIV;
          const RV y = floats(value, cur.n);
          RV r = cur;
          for (int i = 0; i < cur.n; ++i) r.id[i] = ir_.op(o, cur.id[i], y.id[i]);
          value = r;
        }
        assign_to(s->target.get(), convert(value, s->target->type));
        return false;
      }
      case Stmt::CALL:
        lower_expr(s->value.get());
        return false;
      case Stmt::IF: return lower_if(s);
      case Stmt::FOR: lower_for(s); return false;
      case Stmt::BREAK:
      case Stmt::CONTINUE: {
        Loop& L = frame().loops.back();
        const int exec = ir_.and_(frame().path, L.iter);
        if (s->k == Stmt::BREAK) L.brk = ir_.and_(L.brk, ir_.not_(exec));
        L.iter = ir_.and_(L.iter, ir_.not_(exec));
        return true;
      }
      case Stmt::RETURN: {
        Frame& fr = frame();
        const int exec = ir_.and_(ir_.and_(fr.path, fr.ret_alive), cum_alive());
        if (s->value) {
          const RV v = convert(lower_expr(s->value.get()), fr.retval.t);
          for (int i = 0; i < fr.retval.n; ++i) fr.retval.id[i] = ir_.op(OP_SEL, exec, v.id[i], fr.retval.id[i]);
        }
        fr.ret_alive = ir_.and_(fr.ret_alive, ir_.not_(exec));
        return true;
      }
    }
    return false;
  }

  bool lower_if(const Stmt* s) {
    const int c = lower_expr(s->cond.get()).id[0];
    if (ir_.is_const(c)) {
      const Stmt* taken = ir_.is_true(c) ? s->then_s.get() : s->else_s.get();
      return taken ? lower_stmt(taken) : false;
    }
    charge(2 * static_cast<int64_t>(env_.size()), s->line, s->col);  // the two snapshots below
    Frame& fr = frame();
    const int outer_path = fr.path;
    const auto before = env_;
    fr.path = ir_.and_(outer_path, c);
    const bool then_done = lower_stmt(s->then_s.get());
    auto then_env = env_;
    env_ = before;
    bool else_done = false;
    if (s->else_s) {
      frame().path = ir_.and_(outer_path, ir_.not_(c));
      else_done = lower_stmt(s->else_s.get());
    }
    frame().path = outer_path;
    // Merge every variable that existed before the if.
    for (auto& kv : env_) {
      auto it = then_env.find(kv.first);
      if (it == then_env.end()) continue;
      RV& dst = kv.second.val;
      const RV& tv = it->second.val;
      for (int i = 0; i < dst.n; ++i)
        if (tv.id[i] != dst.id[i]) dst.id[i] = ir_.op(OP_SEL, c, tv.id[i], dst.id[i]);
    }
    return then_done && else_done;
  }

  void lower_for(const Stmt* s) {
    const int64_t start = const_int(s->for_init.get(), "the loop start");
    const int64_t bound = const_int(s->for_bound.get(), "the loop bound");
    int64_t step_ = 1;
    if (s->for_step) {
      step_ = const_int(s->for_step.get(), "the loop step");
      if (step_ <= 0) fail(s->line, s->col, "the loop step must be positive");
    }
    const int64_t delta = step_ * s->for_dir;
    auto holds = [&](int64_t i) {
      switch (s->for_cmp) {
        case T_LT: return i < bound;
        case T_LE: return i <= bound;
        case T_GT: return i > bound;
        case T_GE: return i >= bound;
        default: return i != bound;
      }
    };
    int64_t trips = 0;
    for (int64_t i = start; holds(i); i += delta) {
      if (++trips <= k_max_trip) continue;
      // Over the limit: say whether it ever ends, and how often it runs.
      const int64_t span = bound - start;
      bool ends = false;
      switch (s->for_cmp) {
        case T_LT: case T_LE: ends = delta > 0; break;
        case T_GT: case T_GE: ends = delta < 0; break;
        default: ends = span % delta == 0 && span / delta >= 0; break;
      }
      if (!ends) fail(s->line, s->col, "the loop does not terminate");
      int64_t n = 0;
      switch (s->for_cmp) {
        case T_LT: n = (span + delta - 1) / delta; break;
        case T_LE: n = span / delta + 1; break;
        case T_GT: n = (-span - delta - 1) / -delta; break;
        case T_GE: n = -span / -delta + 1; break;
        default: n = span / delta; break;
      }
      fail(s->line, s->col, "loop runs " + std::to_string(n) + " times; the limit is " + std::to_string(k_max_trip));
    }
    if (loop_nesting_ + 1 > k_max_loop_nesting)
      fail(s->line, s->col, "loops are nested more than " + std::to_string(k_max_loop_nesting) + " deep");
    if (trip_product_ * trips > k_max_trip_product)
      fail(s->line, s->col, "nested loops run " + std::to_string(trip_product_ * trips) +
                                " times in total; the limit is " + std::to_string(k_max_trip_product));
    if (trips == 0) return;
    ++loop_nesting_;
    const int64_t saved_product = trip_product_;
    trip_product_ *= trips;
    Loop L;
    L.brk = cum_alive();
    frame().loops.push_back(L);
    int64_t i = start;
    for (int64_t k = 0; k < trips; ++k, i += delta) {
      Loop& cur = frame().loops.back();
      if (ir_.is_false(cur.brk)) break;  // every pixel has left the loop
      cur.iter = cur.brk;
      VarState st;
      st.val = int_val(i);
      st.depth = static_cast<int>(frame().loops.size());
      env_[s->counter] = st;
      lower_stmt(s->then_s.get());
    }
    frame().loops.pop_back();
    trip_product_ = saved_product;
    --loop_nesting_;
  }

  IR& ir_;
  const Parser& ps_;
  std::unordered_map<const VarDecl*, VarState> env_;
  std::unordered_map<const VarDecl*, RV> globals_;
  // A deque, so a call inlined while a statement holds frame() (a return
  // value that calls a function) cannot move the frame it refers to.
  std::deque<Frame> frames_;
  int const_mode_ = 0;
  int call_depth_ = 0;
  int depth_ = 0;
  int64_t steps_ = 0;
  int loop_nesting_ = 0;
  int64_t trip_product_ = 1;
};

// ---------------------------------------------------------------------------
// Finalisation: dead-code removal, the draw / pixel split, register allocation
// ---------------------------------------------------------------------------

// One register allocation of the pixel program. A linear scan; a destination
// never shares a register with its own operands, so the kernels may assume no
// aliasing. Constants and draw values reach registers through OP_LOAD from the
// pool: all of them up front when `pin` is set, else right before first use.
struct PixelAlloc {
  std::vector<Ins> code;
  std::vector<Program::PoolEntry> pool;
  int prologue = 0;  // leading OP_LOADs that run once per draw
  int regs = 0, peak = 0, real_ops = 0, transcendentals = 0;
  uint16_t px = 0, py = 0, out[4] = {0, 0, 0, 0};
};

PixelAlloc allocate_pixels(const IR& ir, const std::vector<char>& live, const std::vector<int>& draw_reg,
                           const int out[4], bool pin) {
  const size_t n = ir.v.size();
  PixelAlloc r;
  std::vector<int> pix_reg(n, -1), pool_index(n, -1), last_use(n, -1);
  std::vector<char> pinned(n, 0);
  std::vector<uint16_t> free_regs;
  int next_reg = 0, live_now = 0;
  auto alloc = [&]() -> uint16_t {
    ++live_now;
    r.peak = std::max(r.peak, live_now);
    if (!free_regs.empty()) {
      const uint16_t reg = free_regs.back();
      free_regs.pop_back();
      return reg;
    }
    return static_cast<uint16_t>(next_reg++);
  };
  for (int k = 0; k < 2; ++k) {
    const uint16_t reg = alloc();
    pix_reg[static_cast<size_t>(ir.input_ids[k])] = reg;
    (k == 0 ? r.px : r.py) = reg;
  }
  auto pixel_ins = [&](size_t i) { return live[i] && ir.v[i].kind == IR::K_INS && ir.v[i].varying; };
  for (size_t i = 0; i < n; ++i) {
    if (!pixel_ins(i)) continue;
    for (int s : {ir.v[i].a, ir.v[i].b, ir.v[i].c})
      if (s >= 0) last_use[static_cast<size_t>(s)] = static_cast<int>(i);
  }
  for (int c = 0; c < 4; ++c) last_use[static_cast<size_t>(out[c])] = static_cast<int>(n);  // live to the end
  // Puts a non-varying value into a register.
  auto load = [&](int id) {
    if (pix_reg[static_cast<size_t>(id)] >= 0) return;
    const IR::Val& v = ir.v[static_cast<size_t>(id)];
    if (pool_index[static_cast<size_t>(id)] < 0) {
      Program::PoolEntry e;
      if (v.kind == IR::K_CONST) e.value = v.value;
      else e.draw_reg = draw_reg[static_cast<size_t>(id)];
      pool_index[static_cast<size_t>(id)] = static_cast<int>(r.pool.size());
      r.pool.push_back(e);
    }
    Ins in;
    in.op = OP_LOAD;
    in.d = alloc();
    in.a = in.b = in.c = static_cast<uint16_t>(pool_index[static_cast<size_t>(id)]);
    pix_reg[static_cast<size_t>(id)] = in.d;
    r.code.push_back(in);
  };
  if (pin) {
    for (size_t i = 0; i < n; ++i) {
      if (!pixel_ins(i)) continue;
      for (int s : {ir.v[i].a, ir.v[i].b, ir.v[i].c})
        if (s >= 0 && !ir.v[static_cast<size_t>(s)].varying) { load(s); pinned[static_cast<size_t>(s)] = 1; }
    }
    for (int c = 0; c < 4; ++c)
      if (!ir.v[static_cast<size_t>(out[c])].varying) { load(out[c]); pinned[static_cast<size_t>(out[c])] = 1; }
    r.prologue = static_cast<int>(r.code.size());
  }
  for (size_t i = 0; i < n; ++i) {
    if (!pixel_ins(i)) continue;
    const IR::Val& v = ir.v[i];
    const int operands[3] = {v.a, v.b, v.c};
    for (int s : operands)
      if (s >= 0 && !ir.v[static_cast<size_t>(s)].varying) load(s);
    Ins in;
    in.op = v.op;
    in.d = alloc();
    pix_reg[i] = in.d;
    in.a = static_cast<uint16_t>(pix_reg[static_cast<size_t>(v.a)]);
    in.b = static_cast<uint16_t>(v.b >= 0 ? pix_reg[static_cast<size_t>(v.b)] : in.a);
    in.c = static_cast<uint16_t>(v.c >= 0 ? pix_reg[static_cast<size_t>(v.c)] : in.a);
    r.code.push_back(in);
    ++r.real_ops;
    if (op_is_transcendental(v.op)) ++r.transcendentals;
    for (int k = 0; k < 3; ++k) {
      const int s = operands[k];
      if (s < 0 || ir.v[static_cast<size_t>(s)].kind == IR::K_INPUT || pinned[static_cast<size_t>(s)]) continue;
      bool seen = false;  // an operand used twice is freed once
      for (int j = 0; j < k; ++j) seen = seen || operands[j] == s;
      if (seen || last_use[static_cast<size_t>(s)] != static_cast<int>(i)) continue;
      free_regs.push_back(static_cast<uint16_t>(pix_reg[static_cast<size_t>(s)]));
      pix_reg[static_cast<size_t>(s)] = -1;
      --live_now;
    }
  }
  for (int c = 0; c < 4; ++c)
    if (!ir.v[static_cast<size_t>(out[c])].varying) load(out[c]);
  for (int c = 0; c < 4; ++c) r.out[c] = static_cast<uint16_t>(pix_reg[static_cast<size_t>(out[c])]);
  r.regs = next_reg;
  return r;
}

// NEUI_DEBUG_SHADER_PIN=0 / 1 forces load-on-demand / pinned constants (for
// measuring the two policies); unset leaves the choice to finalize().
int debug_pin_policy() {
  char buf[8] = {0};
#if defined(_MSC_VER)
  size_t len = 0;
  if (getenv_s(&len, buf, sizeof(buf), "NEUI_DEBUG_SHADER_PIN") != 0 || len == 0) return -1;
#else
  const char* v = std::getenv("NEUI_DEBUG_SHADER_PIN");
  if (!v) return -1;
  std::snprintf(buf, sizeof(buf), "%s", v);
#endif
  return buf[0] == '0' ? 0 : buf[0] == '1' ? 1 : -1;
}

void finalize(const IR& ir, const int out[4], int main_line, int main_col, Program& pg) {
  const size_t n = ir.v.size();
  std::vector<char> live(n, 0);
  std::vector<int> work(out, out + 4);
  while (!work.empty()) {
    const int id = work.back();
    work.pop_back();
    if (live[static_cast<size_t>(id)]) continue;
    live[static_cast<size_t>(id)] = 1;
    const IR::Val& v = ir.v[static_cast<size_t>(id)];
    if (v.kind != IR::K_INS) continue;
    for (int s : {v.a, v.b, v.c})
      if (s >= 0) work.push_back(s);
  }

  // Draw program: every live value that does not depend on the pixel.
  std::vector<int> draw_reg(n, -1);
  int draw_regs = 0;
  for (size_t i = 0; i < n; ++i) {
    const IR::Val& v = ir.v[i];
    if (!live[i] || v.varying) continue;
    const uint16_t r = static_cast<uint16_t>(draw_regs++);
    draw_reg[i] = r;
    switch (v.kind) {
      case IR::K_CONST: pg.draw_consts.push_back({r, v.value}); break;
      case IR::K_UNIFORM: pg.draw_uniforms.push_back({r, static_cast<uint16_t>(v.slot)}); break;
      case IR::K_RESERVED:
        pg.draw_reserved.push_back({r, static_cast<uint8_t>(v.slot)});
        if (v.slot == RES_TIME) pg.flag_bits |= NSL_READS_TIME;
        break;
      case IR::K_INS: {
        Ins in;
        in.op = v.op;
        in.d = r;
        in.a = static_cast<uint16_t>(draw_reg[static_cast<size_t>(v.a)]);
        in.b = static_cast<uint16_t>(v.b >= 0 ? draw_reg[static_cast<size_t>(v.b)] : in.a);
        in.c = static_cast<uint16_t>(v.c >= 0 ? draw_reg[static_cast<size_t>(v.c)] : in.a);
        pg.draw_code.push_back(in);
        break;
      }
      default: break;
    }
  }
  pg.draw_regs = draw_regs;

  // Pixel program. Two allocations: with every constant and draw value
  // pinned (loaded once per draw by a prologue of OP_LOADs), and with each one
  // loaded right before its first use and freed after its last. The second
  // needs the fewest registers and decides the live-value limit; the first is
  // faster and is used whenever it fits within that limit.
  PixelAlloc tight = allocate_pixels(ir, live, draw_reg, out, false);
  const int ops = tight.real_ops + static_cast<int>(pg.draw_code.size());
  if (ops > k_max_ops)
    fail(main_line, main_col, "shader needs " + std::to_string(ops) + " operations after inlining and unrolling; the limit is " + std::to_string(k_max_ops));
  if (tight.peak > k_max_live)
    fail(main_line, main_col, "shader needs " + std::to_string(tight.peak) +
                                  " live values per pixel after inlining and unrolling; the limit is " + std::to_string(k_max_live));
  PixelAlloc pinned = allocate_pixels(ir, live, draw_reg, out, true);
  const int policy = debug_pin_policy();
  PixelAlloc& use = (policy != 0 && pinned.peak <= k_max_live) ? pinned : tight;
  pg.pixel_code = std::move(use.code);
  pg.pixel_pool = std::move(use.pool);
  pg.pixel_prologue = use.prologue;
  pg.pixel_regs = use.regs;
  pg.px = use.px;
  pg.py = use.py;
  for (int c = 0; c < 4; ++c) pg.out[c] = use.out[c];
  pg.stat.pixel_ops = use.real_ops;
  pg.stat.draw_ops = static_cast<int>(pg.draw_code.size());
  pg.stat.pixel_regs = pg.pixel_regs;
  pg.stat.transcendentals = use.transcendentals;
}

}  // namespace
}  // namespace detail

CompileResult compile(const char* src, size_t len, const char* name) {
  using namespace detail;
  CompileResult res;
  const std::string label = name && *name ? name : "<source>";
  FpEnvGuard fp;  // constant folding must not see a host's flush-to-zero
  try {
    if (!src) src = "";
    if (len > k_max_source)
      fail(1, 1, "source is larger than " + std::to_string(k_max_source / 1024) + " KiB");
    uint32_t flag_bits = 0;
    std::vector<Token> toks = Lexer(src, len).run(flag_bits);
    Parser ps(std::move(toks), flag_bits);
    ps.parse_program();

    auto pg = std::make_shared<Program>();
    IR ir;
    Lowerer lw(ir, ps);
    lw.lower_globals();
    // Uniform layout + defaults.
    for (const VarDecl* u : ps.uniforms()) {
      UniformDesc d;
      d.name = u->name;
      d.components = ty_comps(u->type);
      d.offset = u->slot;
      if (u->init) {
        const RV r = lw.uniform_default(u);
        for (int i = 0; i < d.components; ++i) d.defaults[i] = ir.cval(r.id[r.n == 1 ? 0 : i]);
      }
      pg->uniform_list.push_back(d);
    }
    pg->uniform_count = ps.uniform_values();
    pg->flag_bits = ps.flag_bits();
    int out[4];
    try {
      lw.lower_main(out);
    } catch (const CompileError& e) {
      if (e.line == 0) fail(ps.main_fn()->line, ps.main_fn()->col, "shader is too large");
      throw;
    }
    finalize(ir, out, ps.main_fn()->line, ps.main_fn()->col, *pg);
    res.program = std::move(pg);
  } catch (const CompileError& e) {
    res.line = e.line;
    res.col = e.col;
    res.error = label + ":" + std::to_string(e.line) + ":" + std::to_string(e.col) + ": error: " + e.msg;
  } catch (const std::bad_alloc&) {
    res.line = 1;
    res.col = 1;
    res.error = label + ":1:1: error: out of memory compiling the shader";
  }
  return res;
}

const std::vector<UniformDesc>& uniforms(const Program& p) { return p.uniform_list; }
int uniform_components(const Program& p) { return p.uniform_count; }
void default_uniforms(const Program& p, float* values) {
  for (const UniformDesc& u : p.uniform_list)
    for (int i = 0; i < u.components; ++i) values[u.offset + i] = u.defaults[i];
}
uint32_t flags(const Program& p) { return p.flag_bits; }
Stats stats(const Program& p) { return p.stat; }

}  // namespace nsl
}  // namespace neui
