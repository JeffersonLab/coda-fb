/**
 * Minimal test harness for coda-fb tests (no extra dependencies).
 *
 *   TEST(name) { CHECK(cond); CHECK_EQ(a, b); }
 *   int main() { return runAllTests(); }
 *
 * Copyright (c) 2024, Jefferson Science Associates
 */

#ifndef CODA_FB_TEST_HARNESS_HPP
#define CODA_FB_TEST_HARNESS_HPP

#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failures() {
    static int n = 0;
    return n;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline int runAllTests() {
    int failedTests = 0;
    for (auto& t : registry()) {
        int before = failures();
        std::cout << "[ RUN      ] " << t.name << std::endl;
        try {
            t.fn();
        } catch (const std::exception& e) {
            std::cerr << "  unexpected exception: " << e.what() << std::endl;
            failures()++;
        }
        bool ok = failures() == before;
        if (!ok) failedTests++;
        std::cout << (ok ? "[       OK ] " : "[  FAILED  ] ") << t.name << std::endl;
    }
    std::cout << registry().size() - failedTests << "/" << registry().size() << " tests passed" << std::endl;
    return failedTests == 0 ? 0 : 1;
}

}  // namespace testing

#define TEST(name)                                                         \
    static void name();                                                    \
    static testing::Registrar name##_registrar(#name, name);               \
    static void name()

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::cerr << "  " << __FILE__ << ":" << __LINE__ << ": CHECK(" #cond ") failed" \
                      << std::endl;                                                       \
            testing::failures()++;                                                        \
        }                                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        auto va_ = (a);                                                                    \
        auto vb_ = (b);                                                                    \
        if (!(va_ == vb_)) {                                                               \
            std::cerr << "  " << __FILE__ << ":" << __LINE__ << ": CHECK_EQ(" #a ", " #b    \
                      << ") failed: " << va_ << " != " << vb_ << std::endl;                \
            testing::failures()++;                                                         \
        }                                                                                  \
    } while (0)

#endif  // CODA_FB_TEST_HARNESS_HPP
