// Minimalistisch testraamwerk - geen externe dependency nodig.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

namespace testing {

inline int& failures() {
    static int f = 0;
    return f;
}
inline int& checks() {
    static int c = 0;
    return c;
}
inline const char*& currentTest() {
    static const char* t = "";
    return t;
}

inline void reportFailure(const char* file, int line, const std::string& what) {
    ++failures();
    std::fprintf(stderr, "  FAIL %s:%d in [%s]: %s\n", file, line, currentTest(), what.c_str());
}

inline int summary(const char* suite) {
    if (failures() == 0) {
        std::printf("[OK] %s: %d checks geslaagd\n", suite, checks());
        return 0;
    }
    std::printf("[FOUT] %s: %d van %d checks mislukt\n", suite, failures(), checks());
    return 1;
}

} // namespace testing

#define TEST(name) ::testing::currentTest() = name

#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++::testing::checks();                                                       \
        if (!(cond)) ::testing::reportFailure(__FILE__, __LINE__, #cond);            \
    } while (0)

#define CHECK_EQ(a, b)                                                               \
    do {                                                                             \
        ++::testing::checks();                                                       \
        auto va_ = (a);                                                              \
        auto vb_ = (decltype(va_))(b);                                               \
        if (!(va_ == vb_)) {                                                         \
            char buf_[256];                                                          \
            std::snprintf(buf_, sizeof(buf_), "%s == %s  (0x%llx vs 0x%llx)", #a, #b, \
                          (unsigned long long)va_, (unsigned long long)vb_);         \
            ::testing::reportFailure(__FILE__, __LINE__, buf_);                      \
        }                                                                            \
    } while (0)
