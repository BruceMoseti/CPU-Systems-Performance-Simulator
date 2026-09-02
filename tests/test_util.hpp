// Tiny test harness.
//
// A full framework would be a heavier dependency than the code under test, and
// this project needs only registration plus a few assertions.
#pragma once

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& tests() {
  static std::vector<TestCase> registry;
  return registry;
}

inline int& failure_count() {
  static int failures = 0;
  return failures;
}

inline int run_all() {
  int failed_tests = 0;
  for (const TestCase& test : tests()) {
    const int before = failure_count();
    test.fn();
    const bool ok = failure_count() == before;
    if (!ok) ++failed_tests;
    std::cout << (ok ? "PASS " : "FAIL ") << test.name << "\n";
  }
  std::cout << "\n"
            << tests().size() - static_cast<size_t>(failed_tests) << "/" << tests().size()
            << " tests passed\n";
  return failed_tests == 0 ? 0 : 1;
}

}  // namespace testing

#define TEST(test_name)                                              \
  static void test_name();                                           \
  [[maybe_unused]] static const bool test_name##_registered =        \
      (::testing::tests().push_back({#test_name, test_name}), true); \
  static void test_name()

#define CHECK(condition)                                                                       \
  do {                                                                                         \
    if (!(condition)) {                                                                        \
      ++::testing::failure_count();                                                            \
      std::cerr << "  " << __FILE__ << ":" << __LINE__ << ": " << #condition << " is false\n"; \
    }                                                                                          \
  } while (false)

#define CHECK_EQ(actual, expected)                                                               \
  do {                                                                                           \
    const auto check_actual = (actual);                                                          \
    const auto check_expected = (expected);                                                      \
    if (!(check_actual == check_expected)) {                                                     \
      ++::testing::failure_count();                                                              \
      std::cerr << "  " << __FILE__ << ":" << __LINE__ << ": " << #actual << " == " << #expected \
                << " (got " << check_actual << ", want " << check_expected << ")\n";             \
    }                                                                                            \
  } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                                  \
  do {                                                                                           \
    const double check_actual = static_cast<double>(actual);                                     \
    const double check_expected = static_cast<double>(expected);                                 \
    if (std::fabs(check_actual - check_expected) > (tolerance)) {                                \
      ++::testing::failure_count();                                                              \
      std::cerr << "  " << __FILE__ << ":" << __LINE__ << ": " << #actual << " ~= " << #expected \
                << " (got " << check_actual << ", want " << check_expected << ")\n";             \
    }                                                                                            \
  } while (false)

#define CHECK_THROWS(statement)                                              \
  do {                                                                       \
    bool threw = false;                                                      \
    try {                                                                    \
      statement;                                                             \
    } catch (const std::exception&) {                                        \
      threw = true;                                                          \
    }                                                                        \
    if (!threw) {                                                            \
      ++::testing::failure_count();                                          \
      std::cerr << "  " << __FILE__ << ":" << __LINE__ << ": " << #statement \
                << " did not throw\n";                                       \
    }                                                                        \
  } while (false)
