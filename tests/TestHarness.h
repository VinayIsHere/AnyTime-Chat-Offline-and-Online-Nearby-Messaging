// tests/TestHarness.h
//
// A ~60-line test harness, deliberately dependency-free.
//
// GoogleTest would mean fetching and cross-compiling a dependency for every
// target including Android. For a suite this size that cost buys nothing: what
// the tests need is `check(condition, name)`, a pass/fail count and a non-zero
// exit code.
//
// Swap in GoogleTest the day the suite is large enough to want fixtures and
// parameterised cases.

#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace anytime::test {

struct Registry {
    int  passed = 0;
    int  failed = 0;
    std::vector<std::string> failures;

    void check(bool condition, const std::string& name)
    {
        if (condition) {
            ++passed;
            std::printf("    ok   %s\n", name.c_str());
        } else {
            ++failed;
            failures.push_back(name);
            std::printf("  FAIL   %s\n", name.c_str());
        }
    }

    void section(const std::string& title) { std::printf("\n  %s\n", title.c_str()); }

    int report() const
    {
        std::printf("\n  %d passed, %d failed\n", passed, failed);
        if (!failures.empty()) {
            std::printf("\n  failures:\n");
            for (const std::string& f : failures) std::printf("    - %s\n", f.c_str());
        }
        std::printf("\n");
        return failed == 0 ? 0 : 1;
    }
};

}  // namespace anytime::test
