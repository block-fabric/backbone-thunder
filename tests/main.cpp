#include "test.h"

#include <cstdlib>
#include <cstring>
#include <fstream>

static std::string g_data_dir = BT_TEST_DATA_DIR;

std::string bttest::data_dir()
{
    if (const char* e = std::getenv("BT_TEST_DATA")) return e;
    return g_data_dir;
}

std::string bttest::read_file(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}

int main(int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0, failed_cases = 0;
    for (auto& c : bttest::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        int before = bttest::failures();
        std::fprintf(stderr, "[ RUN  ] %s\n", c.name);
        try {
            c.fn();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "  exception: %s\n", e.what());
            bttest::failures()++;
        }
        bool ok = bttest::failures() == before;
        if (!ok) failed_cases++;
        std::fprintf(stderr, "[ %s ] %s\n", ok ? " OK " : "FAIL", c.name);
        run++;
    }
    std::fprintf(stderr, "%d test cases, %d failed\n", run, failed_cases);
    return failed_cases ? 1 : 0;
}
