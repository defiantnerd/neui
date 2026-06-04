#pragma once

// ---------------------------------------------------------------------------
// neui_test - a tiny, dependency-free unit-test harness.
//
// Deliberately mirrors the doctest macro surface so these tests can be
// transposed onto doctest (or Catch2) later with near-mechanical edits:
//
//   neui_test macro      doctest equivalent
//   ----------------     ------------------------
//   TEST_CASE("name")    TEST_CASE("name")          (identical)
//   CHECK(expr)          CHECK(expr)                (identical)
//   CHECK_FALSE(expr)    CHECK_FALSE(expr)          (identical)
//   CHECK_EQ(a, b)       CHECK_EQ(a, b)             (identical)
//   CHECK_APPROX(a, b)   CHECK(a == doctest::Approx(b))
//   REQUIRE(expr)        REQUIRE(expr)              (identical, aborts case)
//   neui_test::Approx    doctest::Approx
//
// To migrate: drop in doctest.h, replace this include, delete test_main.cpp
// (doctest provides main via DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN), and
// s/neui_test::Approx/doctest::Approx/. The TEST_CASE bodies need no changes.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

namespace neui_test
{
  // ---- Registry + counters (inline so the header is single-TU-safe) --------

  struct TestCase { const char* name; void (*fn)(); };

  inline std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
  inline long& checks_run()    { static long n = 0; return n; }
  inline long& checks_failed() { static long n = 0; return n; }
  inline long& current_failed(){ static long n = 0; return n; }

  // Thrown by REQUIRE to abort the current test case (caught by the runner).
  struct RequireFailed {};

  struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({ name, fn }); }
  };

  // ---- Approx (fuzzy float compare, doctest-compatible spelling) -----------

  struct Approx {
    double value;
    double eps = 1e-5;
    explicit Approx(double v) : value(v) {}
    Approx& epsilon(double e) { eps = e; return *this; }
  };
  inline bool operator==(double lhs, const Approx& rhs) {
    return std::fabs(lhs - rhs.value) <=
           rhs.eps * (1.0 + (std::max)(std::fabs(lhs), std::fabs(rhs.value)));
  }
  inline bool operator==(const Approx& lhs, double rhs) { return rhs == lhs; }
  inline bool operator!=(double lhs, const Approx& rhs) { return !(lhs == rhs); }
  inline bool operator!=(const Approx& lhs, double rhs) { return !(lhs == rhs); }

  // ---- Assertion plumbing --------------------------------------------------

  inline bool report(bool cond, const char* what, const char* expr,
                     const char* file, int line)
  {
    ++checks_run();
    if (cond) return true;
    ++checks_failed();
    ++current_failed();
    std::fprintf(stderr, "  FAIL %s:%d: %s(%s)\n", file, line, what, expr);
    return false;
  }

  template <class A, class B>
  bool report_eq(const A& a, const B& b, const char* ea, const char* eb,
                 const char* file, int line)
  {
    ++checks_run();
    if (a == b) return true;
    ++checks_failed();
    ++current_failed();
    std::ostringstream oss;
    oss << "  FAIL " << file << ":" << line << ": CHECK_EQ(" << ea << ", " << eb
        << ")  [" << a << " != " << b << "]\n";
    std::fputs(oss.str().c_str(), stderr);
    return false;
  }

  // ---- Runner --------------------------------------------------------------

  inline int run_all()
  {
    long passed = 0, failed = 0;
    for (const auto& tc : registry()) {
      current_failed() = 0;
      std::fprintf(stderr, "[ run ] %s\n", tc.name);
      try {
        tc.fn();
      } catch (const RequireFailed&) {
        // REQUIRE already recorded the failure; case is aborted.
      } catch (const std::exception& e) {
        ++current_failed();
        std::fprintf(stderr, "  FAIL %s: uncaught exception: %s\n", tc.name, e.what());
      } catch (...) {
        ++current_failed();
        std::fprintf(stderr, "  FAIL %s: uncaught unknown exception\n", tc.name);
      }
      if (current_failed() == 0) {
        ++passed;
      } else {
        ++failed;
        std::fprintf(stderr, "FAILED: %s\n", tc.name);
      }
    }
    std::fprintf(stdout,
      "\nneui_test: %ld test cases (%ld passed, %ld failed); "
      "%ld checks (%ld failed)\n",
      (long)registry().size(), passed, failed, checks_run(), checks_failed());
    return failed == 0 ? 0 : 1;
  }
} // namespace neui_test

// ---- Macros ---------------------------------------------------------------

#define NEUI_TEST_CAT2(a, b) a##b
#define NEUI_TEST_CAT(a, b)  NEUI_TEST_CAT2(a, b)

#define TEST_CASE(name)                                                        \
  static void NEUI_TEST_CAT(neui_test_fn_, __LINE__)();                        \
  static ::neui_test::Registrar NEUI_TEST_CAT(neui_test_reg_, __LINE__)(       \
      name, &NEUI_TEST_CAT(neui_test_fn_, __LINE__));                          \
  static void NEUI_TEST_CAT(neui_test_fn_, __LINE__)()

#define CHECK(expr)        ::neui_test::report((expr), "CHECK", #expr, __FILE__, __LINE__)
#define CHECK_FALSE(expr)  ::neui_test::report(!(expr), "CHECK_FALSE", #expr, __FILE__, __LINE__)
#define CHECK_EQ(a, b)     ::neui_test::report_eq((a), (b), #a, #b, __FILE__, __LINE__)
#define CHECK_APPROX(a, b) \
  ::neui_test::report((a) == ::neui_test::Approx(b), "CHECK_APPROX", #a " == " #b, __FILE__, __LINE__)

#define REQUIRE(expr)                                                          \
  do {                                                                         \
    if (!::neui_test::report((expr), "REQUIRE", #expr, __FILE__, __LINE__))    \
      throw ::neui_test::RequireFailed{};                                      \
  } while (0)
