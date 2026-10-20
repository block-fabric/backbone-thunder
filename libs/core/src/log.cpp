// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <optional>

#include <bt/log.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>

namespace bt::log {

static std::mutex g_mu;
static Level g_console = Level::Info;
static Level g_file_level = Level::Warn;
static std::string g_dir;
static std::string g_day;
static FILE* g_file = nullptr;

std::optional<Level> parse_level(const std::string& s)
{
    std::string l;
    for (char c : s) l.push_back(char(std::tolower((unsigned char)c)));
    if (l == "error") return Level::Error;
    if (l == "warn" || l == "warning") return Level::Warn;
    if (l == "info") return Level::Info;
    if (l == "debug") return Level::Debug;
    if (l == "trace") return Level::Trace;
    return std::nullopt;
}

void set_console_level(Level l)
{
    std::lock_guard<std::mutex> g(g_mu);
    g_console = l;
}

void set_file(const std::string& dir, Level l)
{
    std::lock_guard<std::mutex> g(g_mu);
    g_dir = dir;
    g_file_level = l;
    if (!dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }
}

static const char* name(Level l)
{
    switch (l) {
    case Level::Error: return "ERROR";
    case Level::Warn: return " WARN";
    case Level::Info: return " INFO";
    case Level::Debug: return "DEBUG";
    case Level::Trace: return "TRACE";
    }
    return "";
}

void write(Level l, const char* fmt, ...)
{
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char ts[32], day[16];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);
    std::strftime(day, sizeof(day), "%Y-%m-%d", &tm);
    std::lock_guard<std::mutex> g(g_mu);
    if (l <= g_console) std::fprintf(stderr, "%s %s %s\n", ts, name(l), msg);
    if (!g_dir.empty() && l <= g_file_level) {
        if (g_day != day || !g_file) {
            if (g_file) std::fclose(g_file);
            g_day = day;
            g_file = std::fopen((g_dir + "/" + day + ".log").c_str(), "a");
        }
        if (g_file) {
            std::fprintf(g_file, "%s %s %s\n", ts, name(l), msg);
            std::fflush(g_file);
        }
    }
}

} // namespace bt::log
