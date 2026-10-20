// Minimal test harness (no third-party dependency).
#pragma once

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace bttest {
struct Case {
    const char* name;
    std::function<void()> fn;
};
inline std::vector<Case>& registry()
{
    static std::vector<Case> r;
    return r;
}
struct Reg {
    Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};
struct Failure {
    std::string msg;
};
inline int& failures()
{
    static int f = 0;
    return f;
}
/** Directory holding tests/data (set from argv or BT_TEST_DATA). */
std::string data_dir();
std::string read_file(const std::string& path);
} // namespace bttest

#define BT_CAT2(a, b) a##b
#define BT_CAT(a, b) BT_CAT2(a, b)
#define TEST(name)                                                    \
    static void name();                                               \
    static bttest::Reg BT_CAT(reg_, name)(#name, name);               \
    static void name()

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "  CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            bttest::failures()++;                                                          \
        }                                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                                                      \
    do {                                                                                                    \
        auto _va = (a);                                                                                     \
        auto _vb = (b);                                                                                     \
        if (!(_va == _vb)) {                                                                                \
            std::ostringstream _o;                                                                          \
            _o << _va << " != " << _vb;                                                                     \
            std::fprintf(stderr, "  CHECK_EQ failed %s:%d: %s == %s (%s)\n", __FILE__, __LINE__, #a, #b, \
                         _o.str().c_str());                                                                 \
            bttest::failures()++;                                                                           \
        }                                                                                                   \
    } while (0)

#define REQUIRE(cond)                                                                        \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "  REQUIRE failed %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            bttest::failures()++;                                                            \
            return;                                                                          \
        }                                                                                    \
    } while (0)
