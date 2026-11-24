#pragma once

// Minimal header-only unit test framework for vrc_unit_tests.
//
//   VRC_TEST(name) { ... }        defines and self-registers a test case
//   VRC_CHECK(cond)               records a failure (file:line + expression)
//                                 when cond is false, then continues
//   VRC_CHECK_NEAR(a, b, eps)     records a failure when |a - b| > eps (or
//                                 either value is NaN), then continues
//
// Test names must be valid identifiers and unique within one .cpp file; the
// same name in different files is fine (the functions have internal linkage).
// tests/unit/test_main.cpp runs every registered test.

#include <cmath>
#include <cstdio>
#include <vector>

namespace vrc_test {

using TestFn = void (*)();

struct TestCase {
    const char* name;
    TestFn fn;
    const char* file;
    int line;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, TestFn fn, const char* file, int line) {
        registry().push_back({ name, fn, file, line });
    }
};

// Per-run counters. failures_in_current_test is reset by the runner before
// each test.
struct Stats {
    int checks = 0;
    int failed_checks = 0;
    int failures_in_current_test = 0;
};

inline Stats& stats() {
    static Stats s;
    return s;
}

inline void check(bool ok, const char* expr, const char* file, int line) {
    Stats& s = stats();
    ++s.checks;
    if (ok) return;
    ++s.failed_checks;
    ++s.failures_in_current_test;
    std::printf("    %s(%d): CHECK failed: %s\n", file, line, expr);
}

inline void check_near(double a, double b, double eps,
                       const char* a_expr, const char* b_expr, const char* eps_expr,
                       const char* file, int line) {
    Stats& s = stats();
    ++s.checks;
    const double diff = std::fabs(a - b);
    // Written so that NaN in a, b or eps fails the check.
    if (diff <= eps) return;
    ++s.failed_checks;
    ++s.failures_in_current_test;
    std::printf("    %s(%d): CHECK_NEAR failed: %s ~= %s (eps %s)\n"
                "        actual: %.9g vs %.9g, |diff| = %.9g > %.9g\n",
                file, line, a_expr, b_expr, eps_expr, a, b, diff, eps);
}

} // namespace vrc_test

#define VRC_TEST(name)                                                        \
    static void vrc_test_fn_##name();                                         \
    static const ::vrc_test::Registrar vrc_test_registrar_##name(             \
        #name, &vrc_test_fn_##name, __FILE__, __LINE__);                      \
    static void vrc_test_fn_##name()

#define VRC_CHECK(cond)                                                       \
    ::vrc_test::check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

#define VRC_CHECK_NEAR(a, b, eps)                                             \
    ::vrc_test::check_near(static_cast<double>(a), static_cast<double>(b),    \
                           static_cast<double>(eps), #a, #b, #eps,            \
                           __FILE__, __LINE__)
