// Runner for vrc_unit_tests: executes every VRC_TEST registered by the other
// tests/unit/*.cpp files and returns non-zero if any check failed.
//
// Usage: vrc_unit_tests [substring]   (optional filter on test names)

#include "test_framework.h"

#include <cstdio>
#include <cstring>
#include <exception>

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;

    auto& tests = vrc_test::registry();
    auto& stats = vrc_test::stats();

    int run = 0;
    int failed = 0;
    for (const auto& t : tests) {
        if (filter && !std::strstr(t.name, filter)) continue;
        ++run;
        stats.failures_in_current_test = 0;
        std::printf("[ RUN  ] %s\n", t.name);
        try {
            t.fn();
        } catch (const std::exception& e) {
            ++stats.failures_in_current_test;
            std::printf("    %s(%d): unhandled exception: %s\n", t.file, t.line, e.what());
        } catch (...) {
            ++stats.failures_in_current_test;
            std::printf("    %s(%d): unhandled non-standard exception\n", t.file, t.line);
        }
        if (stats.failures_in_current_test == 0) {
            std::printf("[  OK  ] %s\n", t.name);
        } else {
            ++failed;
            std::printf("[ FAIL ] %s (%d failure%s)\n", t.name,
                        stats.failures_in_current_test,
                        stats.failures_in_current_test == 1 ? "" : "s");
        }
    }

    std::printf("\n%d test%s run, %d passed, %d failed; %d checks, %d failed\n",
                run, run == 1 ? "" : "s", run - failed, failed,
                stats.checks, stats.failed_checks);

    if (run == 0) {
        std::printf("No tests matched%s%s.\n", filter ? " filter " : "", filter ? filter : "");
        return 1;
    }
    return failed == 0 ? 0 : 1;
}
