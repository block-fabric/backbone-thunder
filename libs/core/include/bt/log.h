// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Leveled logging to stderr and to a daily rolling file (<dir>/YYYY-MM-DD.log, the
// layout tracing_appender's DAILY rotation with suffix "log" produces).
#pragma once

#include <cstdarg>
#include <optional>
#include <string>

namespace bt::log {

enum class Level { Error = 0, Warn, Info, Debug, Trace };

std::optional<Level> parse_level(const std::string& s);
void set_console_level(Level l);
void set_file(const std::string& dir, Level l); // empty dir: no file logging
void write(Level l, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

} // namespace bt::log

#define LOG_ERROR(...) ::bt::log::write(::bt::log::Level::Error, __VA_ARGS__)
#define LOG_WARN(...) ::bt::log::write(::bt::log::Level::Warn, __VA_ARGS__)
#define LOG_INFO(...) ::bt::log::write(::bt::log::Level::Info, __VA_ARGS__)
#define LOG_DEBUG(...) ::bt::log::write(::bt::log::Level::Debug, __VA_ARGS__)
